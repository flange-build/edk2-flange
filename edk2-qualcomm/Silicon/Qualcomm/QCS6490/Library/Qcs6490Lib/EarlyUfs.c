/** @file
  A minimal, polled UFS read path for SEC on QCS6490.

  XBL leaves the UFS host controller, the link and the device up after it
  has loaded UEFI from UFS. SEC reads a few blocks through the controller as
  XBL left it: one UTP transfer request at a time, in a slot of the transfer
  request list XBL left running when that list is in memory UEFI may use, or
  else in a list of its own, stopping XBL's list first. The controller is
  never reset and the link never touched: QcomUfsHcDxe owns the PHY and
  initializes everything again in DXE.

  Reference: UFSHCI 3.0 (JESD223D) and UFS 3.1 (JESD220E); the layout of the
  command descriptor follows MdeModulePkg's UfsPassThruDxe, with the
  response UPIU and the PRDT at the 512-byte offsets the Qualcomm controller
  is known to work with.

  This runs with the MMU off, so every access is to Device memory: the
  descriptors are written and read one aligned 32-bit word or one byte at a
  time.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <IndustryStandard/Scsi.h>
#include <IndustryStandard/UfsHci.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

#include "Qcs6490Early.h"
#include "Qcs6490LibInternal.h"

//
// UFSHCI register bits UfsHci.h does not define.
//
#define UFS_HC_VER_MAJOR(x)  (((x) >> 8) & 0xFF)
#define UFS_HC_VER_MINOR(x)  (((x) >> 4) & 0xF)

#define UFS_HC_HCS_UTRLRDY  BIT1
#define UFS_HC_HCS_READY    (UFS_HC_HCS_DP | UFS_HC_HCS_UTRLRDY | UFS_HC_HCS_UCRDY)

#define UFS_HC_IS_UTRCS  BIT0
#define UFS_HC_IS_ULLS   BIT7
#define UFS_HC_IS_DFES   BIT11
#define UFS_HC_IS_UTPES  BIT12
#define UFS_HC_IS_HCFES  BIT16
#define UFS_HC_IS_SBFES  BIT17
#define UFS_HC_IS_FATAL  \
  (UFS_HC_IS_ULLS | UFS_HC_IS_DFES | UFS_HC_IS_UTPES | UFS_HC_IS_HCFES | UFS_HC_IS_SBFES)

//
// Errors that make SEC leave the controller alone when XBL left them set.
// A lost link or a UTP error flagged earlier only counts if SEC's own
// request raises it again.
//
#define UFS_HC_IS_FATAL_AT_ENTRY  (UFS_HC_IS_DFES | UFS_HC_IS_HCFES | UFS_HC_IS_SBFES)

#define UFS_HC_UTRLBA_MASK  0xFFFFFC00

//
// UTP transfer request descriptor (UFSHCI 3.0 section 6.1.1), as 32-bit
// words.
//
#define UTRD_SIZE                  32
#define UTRD_WORDS                 (UTRD_SIZE / sizeof (UINT32))
#define UTRD_DW0_CT_UFS_STORAGE    (1U << 28)
#define UTRD_DW0_DD_DEVICE_TO_HOST (2U << 25)
#define UTRD_DW0_INTERRUPT         BIT24
#define UTRD_DW2_OCS(x)            ((x) & 0xFF)
#define UTRD_OCS_SUCCESS           0x00

//
// UTP command descriptor: the command UPIU, the response UPIU and the PRDT,
// at offsets counted in 32-bit words in the UTRD.
//
#define UCD_COMMAND_OFFSET    0x000
#define UCD_RESPONSE_OFFSET   0x200
#define UCD_RESPONSE_SIZE     0x200
#define UCD_PRDT_OFFSET       0x400
#define UCD_PRDT_ENTRY_SIZE   16
#define UCD_PRDT_MAX_ENTRIES  16
#define UCD_SIZE              (UCD_PRDT_OFFSET + UCD_PRDT_MAX_ENTRIES * UCD_PRDT_ENTRY_SIZE)

//
// Bytes one PRDT entry may describe, and bytes per read command.
//
#define PRDT_MAX_BYTES       SIZE_256KB
#define READ_MAX_BYTES       SIZE_256KB
#define READ_MAX_BLOCKS      (READ_MAX_BYTES / QCS6490_NVSTORE_BLOCK_SIZE)

//
// UPIUs (UFS 3.1 section 10.7).
//
#define UPIU_TRANSACTION_COMMAND      0x01
#define UPIU_TRANSACTION_RESPONSE     0x21
#define UPIU_TRANSACTION_CODE(x)      ((x) & 0x3F)
#define UPIU_FLAGS_READ               BIT6
#define UPIU_FLAGS_TASK_ATTR_SIMPLE   0x00
#define UPIU_FLAGS_OVERFLOW           BIT6
#define UPIU_FLAGS_UNDERFLOW          BIT5
#define UPIU_COMMAND_SET_SCSI         0x00
#define UPIU_RESPONSE_TARGET_SUCCESS  0x00

//
// Response UPIU fields, as byte offsets.
//
#define RESPONSE_DW0                0x00
#define RESPONSE_DW1                0x04
#define RESPONSE_RESIDUAL           0x0C
#define RESPONSE_SENSE_DATA         0x22

//
// SCSI status codes (SAM-5) and sense data formats (SPC-4).
//
#define SCSI_STATUS_GOOD             0x00
#define SCSI_STATUS_CHECK_CONDITION  0x02
#define SCSI_STATUS_BUSY             0x08
#define SCSI_STATUS_TASK_SET_FULL    0x28
#define SCSI_SENSE_DESCRIPTOR_FORMAT 0x72

//
// Highest LUN a normal logical unit may have.
//
#define UFS_MAX_LUN  7

//
// How long a read may take, how long clearing a request or an idle
// doorbell may take, and how often a read is tried when the device asks to
// retry (unit attention, busy).
//
#define UFS_COMMAND_TIMEOUT_US  1000000
#define UFS_CLEAR_TIMEOUT_US    100000
#define UFS_IDLE_TIMEOUT_US     100000
#define UFS_COMMAND_ATTEMPTS    4

/**
  Records a failure.

  @param[in, out] Ufs         The controller.
  @param[in]      Step        QCS6490_UFS_STEP_*.
  @param[in]      Ocs         The overall command status, or 0.
  @param[in]      ScsiStatus  The SCSI status, or 0.
  @param[in]      Detail      Step-specific detail.
**/
STATIC
VOID
SetDiagnostic (
  IN OUT QCS6490_EARLY_UFS  *Ufs,
  IN     UINT32             Step,
  IN     UINT32             Ocs,
  IN     UINT32             ScsiStatus,
  IN     UINT32             Detail
  )
{
  Ufs->Diagnostic = QCS6490_UFS_DIAG (Step, Ocs, ScsiStatus, Detail);
}

/**
  Packs the fatal error bits of IS into a diagnostic detail byte.

  @param[in]  Is  IS.

  @return  ULLS | DFES << 1 | UTPES << 2 | HCFES << 3 | SBFES << 4.
**/
STATIC
UINT32
FatalBits (
  IN UINT32  Is
  )
{
  return (((Is & UFS_HC_IS_ULLS) != 0) ? BIT0 : 0) |
         (((Is & UFS_HC_IS_DFES) != 0) ? BIT1 : 0) |
         (((Is & UFS_HC_IS_UTPES) != 0) ? BIT2 : 0) |
         (((Is & UFS_HC_IS_HCFES) != 0) ? BIT3 : 0) |
         (((Is & UFS_HC_IS_SBFES) != 0) ? BIT4 : 0);
}

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
  )
{
  UINT32                 Cap;
  UINT32                 Ver;
  UINT32                 Hce;
  UINT32                 Hcs;
  UINT32                 Is;
  UINT32                 Ie;
  UINT32                 Doorbell;
  UINT32                 RunStop;
  UINT32                 ListLow;
  UINT32                 ListHigh;
  UINT32                 Index;
  QCS6490_EARLY_TIMEOUT  Timeout;

  Ufs->Base       = FixedPcdGet32 (PcdQcomUfsHcDxeBaseAddress);
  Ufs->SlotCount  = 0;
  Ufs->ListBase   = 0;
  Ufs->XblList    = FALSE;
  Ufs->Slot       = 0;
  Ufs->TrdSaved   = FALSE;
  Ufs->Ready      = FALSE;
  Ufs->Pending    = FALSE;
  Ufs->Diagnostic = 0;
  for (Index = 0; Index < UTRD_WORDS; Index++) {
    Ufs->SavedTrd[Index] = 0;
  }

  if (Ufs->Base == 0) {
    Qcs6490Print ("QCS6490: UFS: no controller address\n");
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_NOT_RUNNING, 0, 0, 0);
    return RETURN_NOT_FOUND;
  }

  Cap      = MmioRead32 (Ufs->Base + UFS_HC_CAP_OFFSET);
  Ver      = MmioRead32 (Ufs->Base + UFS_HC_VER_OFFSET);
  Hce      = MmioRead32 (Ufs->Base + UFS_HC_ENABLE_OFFSET);
  Hcs      = MmioRead32 (Ufs->Base + UFS_HC_STATUS_OFFSET);
  Is       = MmioRead32 (Ufs->Base + UFS_HC_IS_OFFSET);
  Ie       = MmioRead32 (Ufs->Base + UFS_HC_IE_OFFSET);
  Doorbell = MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET);
  RunStop  = MmioRead32 (Ufs->Base + UFS_HC_UTRLRSR_OFFSET);
  ListLow  = MmioRead32 (Ufs->Base + UFS_HC_UTRLBA_OFFSET);
  ListHigh = MmioRead32 (Ufs->Base + UFS_HC_UTRLBAU_OFFSET);

  Ufs->SlotCount = (Cap & UFS_HC_CAP_NUTRS) + 1;

  //
  // What XBL left, for the log: on the first boots on new boot firmware this
  // is all there is to go by.
  //
  Qcs6490Print (
    "QCS6490: UFS: HCI %u.%u CAP 0x%x HCE 0x%x HCS 0x%x IS 0x%x IE 0x%x\n",
    UFS_HC_VER_MAJOR (Ver),
    UFS_HC_VER_MINOR (Ver),
    Cap,
    Hce,
    Hcs,
    Is,
    Ie
    );
  Qcs6490Print (
    "QCS6490: UFS: list 0x%lx %a, doorbell 0x%x, %u slots, AHIT 0x%x UTRIACR 0x%x\n",
    LShiftU64 (ListHigh, 32) | (ListLow & UFS_HC_UTRLBA_MASK),
    ((RunStop & UFS_HC_UTRLRSR) != 0) ? "running" : "stopped",
    Doorbell,
    Ufs->SlotCount,
    MmioRead32 (Ufs->Base + UFS_HC_AHIT_OFFSET),
    MmioRead32 (Ufs->Base + UFS_HC_UTRIACR_OFFSET)
    );

  if (((Hce & UFS_HC_HCE_EN) == 0) || ((Hcs & UFS_HC_HCS_READY) != UFS_HC_HCS_READY)) {
    Qcs6490Print ("QCS6490: UFS: controller not ready\n");
    SetDiagnostic (
      Ufs,
      QCS6490_UFS_STEP_NOT_RUNNING,
      0,
      0,
      (((Hce & UFS_HC_HCE_EN) != 0) ? BIT0 : 0) |
      (((Hcs & UFS_HC_HCS_DP) != 0) ? BIT1 : 0) |
      (((Hcs & UFS_HC_HCS_UTRLRDY) != 0) ? BIT2 : 0) |
      (((Hcs & UFS_HC_HCS_UCRDY) != 0) ? BIT3 : 0)
      );
    return RETURN_NOT_READY;
  }

  if ((Is & UFS_HC_IS_FATAL_AT_ENTRY) != 0) {
    Qcs6490Print ("QCS6490: UFS: controller reports a fatal error\n");
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_FATAL, 0, 0, FatalBits (Is));
    return RETURN_DEVICE_ERROR;
  }

  //
  // XBL should have nothing outstanding; give it a moment in case it has.
  //
  Qcs6490EarlyTimeoutStart (&Timeout, UFS_IDLE_TIMEOUT_US);
  while (Doorbell != 0) {
    if (Qcs6490EarlyTimeoutExpired (&Timeout)) {
      Qcs6490Print ("QCS6490: UFS: requests outstanding (doorbell 0x%x)\n", Doorbell);
      SetDiagnostic (Ufs, QCS6490_UFS_STEP_BUSY, 0, 0, Doorbell);
      return RETURN_NOT_READY;
    }

    Doorbell = MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET);
  }

  if ((RunStop & UFS_HC_UTRLRSR) != 0) {
    //
    // The base of a running list cannot change: use XBL's list, if SEC may
    // write it and the controller may read it through the SMMU.
    //
    Ufs->ListBase = LShiftU64 (ListHigh, 32) | (ListLow & UFS_HC_UTRLBA_MASK);
    if (Qcs6490EarlyIsFreeDram (Ufs->ListBase, Ufs->SlotCount * UTRD_SIZE) &&
        !((Ufs->ListBase < QCS6490_EARLY_NV_AREA_BASE + QCS6490_EARLY_NV_AREA_SIZE) &&
          (QCS6490_EARLY_NV_AREA_BASE < Ufs->ListBase + Ufs->SlotCount * UTRD_SIZE)))
    {
      Ufs->XblList = TRUE;
      Ufs->Slot    = 0;
      for (Index = 0; Index < UTRD_WORDS; Index++) {
        Ufs->SavedTrd[Index] = MmioRead32 ((UINTN)Ufs->ListBase + Ufs->Slot * UTRD_SIZE + Index * sizeof (UINT32));
      }

      Ufs->TrdSaved = TRUE;
      Ufs->Ready    = TRUE;
      return RETURN_SUCCESS;
    }

    //
    // XBL's list is in XBL's own memory. With nothing outstanding the list
    // may be stopped and moved, as UfsPassThruDxe's UfsControllerStop does
    // (UFSHCI 3.0 section 5.4.5); nothing else about the controller or the
    // link changes.
    //
    MmioWrite32 (Ufs->Base + UFS_HC_UTRLRSR_OFFSET, 0);
    Qcs6490EarlyTimeoutStart (&Timeout, UFS_CLEAR_TIMEOUT_US);
    while ((MmioRead32 (Ufs->Base + UFS_HC_UTRLRSR_OFFSET) & UFS_HC_UTRLRSR) != 0) {
      if (Qcs6490EarlyTimeoutExpired (&Timeout)) {
        Qcs6490Print ("QCS6490: UFS: XBL's list at 0x%lx did not stop\n", Ufs->ListBase);
        SetDiagnostic (Ufs, QCS6490_UFS_STEP_LIST, 0, 0, BIT0);
        return RETURN_ACCESS_DENIED;
      }
    }

    Qcs6490Print ("QCS6490: UFS: stopped XBL's list at 0x%lx\n", Ufs->ListBase);
  }

  //
  // A list of SEC's own, in the scratch memory. It stays programmed until
  // QcomUfsHcDxe resets the controller; nothing rings its doorbell until
  // then.
  //
  Ufs->ListBase = QCS6490_EARLY_UTRL_BASE;
  Ufs->XblList  = FALSE;
  Ufs->Slot     = 0;
  for (Index = 0; Index < Ufs->SlotCount * UTRD_WORDS; Index++) {
    MmioWrite32 ((UINTN)Ufs->ListBase + Index * sizeof (UINT32), 0);
  }

  ArmDataSynchronizationBarrier ();
  MmioWrite32 (Ufs->Base + UFS_HC_UTRLBA_OFFSET, (UINT32)Ufs->ListBase);
  MmioWrite32 (Ufs->Base + UFS_HC_UTRLBAU_OFFSET, (UINT32)RShiftU64 (Ufs->ListBase, 32));

  //
  // A controller that kept the old base would run XBL's stale descriptors.
  //
  if (((MmioRead32 (Ufs->Base + UFS_HC_UTRLBA_OFFSET) & UFS_HC_UTRLBA_MASK) != (UINT32)Ufs->ListBase) ||
      (MmioRead32 (Ufs->Base + UFS_HC_UTRLBAU_OFFSET) != (UINT32)RShiftU64 (Ufs->ListBase, 32)))
  {
    Qcs6490Print ("QCS6490: UFS: the list base did not take 0x%lx\n", Ufs->ListBase);
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_LIST_START, 0, 0, BIT1);
    return RETURN_DEVICE_ERROR;
  }

  MmioWrite32 (Ufs->Base + UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR);
  if ((MmioRead32 (Ufs->Base + UFS_HC_UTRLRSR_OFFSET) & UFS_HC_UTRLRSR) == 0) {
    Qcs6490Print ("QCS6490: UFS: own list at 0x%lx did not start\n", Ufs->ListBase);
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_LIST_START, 0, 0, 0);
    return RETURN_DEVICE_ERROR;
  }

  Qcs6490Print ("QCS6490: UFS: started own list at 0x%lx\n", Ufs->ListBase);

  Ufs->Ready = TRUE;
  return RETURN_SUCCESS;
}

/**
  Tells the controller to drop the request in SEC's slot, after it failed
  to complete.

  @param[in, out] Ufs  The controller.

  @retval TRUE   The slot is free again.
  @retval FALSE  It is not: the controller may still do DMA for it.
**/
STATIC
BOOLEAN
ClearSlot (
  IN OUT QCS6490_EARLY_UFS  *Ufs
  )
{
  UINT32                 Bit;
  QCS6490_EARLY_TIMEOUT  Timeout;

  Bit = BIT0 << Ufs->Slot;

  //
  // UTRLCLR: a 0 clears the slot, a 1 leaves it alone.
  //
  MmioWrite32 (Ufs->Base + UFS_HC_UTRLCLR_OFFSET, ~Bit);

  Qcs6490EarlyTimeoutStart (&Timeout, UFS_CLEAR_TIMEOUT_US);
  while ((MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET) & Bit) != 0) {
    if (Qcs6490EarlyTimeoutExpired (&Timeout)) {
      Ufs->Pending = TRUE;
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Reads the sense key a CHECK CONDITION came with.

  @param[in]  Response  The response UPIU.

  @return  The sense key, or 0 (NO SENSE) if there is no sense data.
**/
STATIC
UINT8
GetSenseKey (
  IN UINTN  Response
  )
{
  UINT32  SenseLength;
  UINT8   ResponseCode;

  //
  // The data segment: a big-endian sense data length, then the sense data.
  //
  SenseLength = ((UINT32)MmioRead8 (Response + RESPONSE_SENSE_DATA - 2) << 8) |
                MmioRead8 (Response + RESPONSE_SENSE_DATA - 1);
  if (SenseLength < 3) {
    return EFI_SCSI_SK_NO_SENSE;
  }

  ResponseCode = MmioRead8 (Response + RESPONSE_SENSE_DATA) & 0x7F;
  if (ResponseCode >= SCSI_SENSE_DESCRIPTOR_FORMAT) {
    return MmioRead8 (Response + RESPONSE_SENSE_DATA + 1) & 0x0F;
  }

  return MmioRead8 (Response + RESPONSE_SENSE_DATA + 2) & 0x0F;
}

/**
  Issues one SCSI READ and waits for it.

  @param[in, out] Ufs     The controller.
  @param[in]      Lun     The LUN.
  @param[in]      Lba     The first block.
  @param[in]      Blocks  The number of blocks, at most READ_MAX_BLOCKS.
  @param[in]      Buffer  Where to put them.

  @retval RETURN_SUCCESS       The blocks were read.
  @retval RETURN_NOT_READY     The device asked to try again.
  @retval RETURN_TIMEOUT       The request did not complete.
  @retval RETURN_DEVICE_ERROR  It failed.
**/
STATIC
RETURN_STATUS
ExecuteRead (
  IN OUT QCS6490_EARLY_UFS  *Ufs,
  IN     UINT8              Lun,
  IN     UINT64             Lba,
  IN     UINT32             Blocks,
  IN     UINT64             Buffer
  )
{
  UINT8                  Cdb[16];
  UINTN                  Ucd;
  UINTN                  Trd;
  UINTN                  Response;
  UINT32                 Length;
  UINT32                 Remaining;
  UINT64                 Address;
  UINT32                 Chunk;
  UINT32                 Entries;
  UINT32                 Index;
  UINT32                 Bit;
  UINT32                 IsBefore;
  UINT32                 Is;
  UINT32                 Clear;
  UINT32                 Ocs;
  UINT32                 ResponseDw0;
  UINT32                 ResponseDw1;
  UINT8                  ScsiStatus;
  UINT8                  SenseKey;
  BOOLEAN                Fatal;
  QCS6490_EARLY_TIMEOUT  Timeout;

  Ucd      = (UINTN)QCS6490_EARLY_UCD_BASE;
  Response = Ucd + UCD_RESPONSE_OFFSET;
  Trd      = (UINTN)Ufs->ListBase + Ufs->Slot * UTRD_SIZE;
  Bit      = BIT0 << Ufs->Slot;
  Length   = Blocks * QCS6490_NVSTORE_BLOCK_SIZE;

  //
  // READ(10) while the block fits, READ(16) beyond.
  //
  for (Index = 0; Index < sizeof (Cdb); Index++) {
    Cdb[Index] = 0;
  }

  if (Lba + Blocks <= MAX_UINT32) {
    Cdb[0] = EFI_SCSI_OP_READ10;
    Cdb[2] = (UINT8)RShiftU64 (Lba, 24);
    Cdb[3] = (UINT8)RShiftU64 (Lba, 16);
    Cdb[4] = (UINT8)RShiftU64 (Lba, 8);
    Cdb[5] = (UINT8)Lba;
    Cdb[7] = (UINT8)(Blocks >> 8);
    Cdb[8] = (UINT8)Blocks;
  } else {
    Cdb[0] = EFI_SCSI_OP_READ16;
    for (Index = 0; Index < 8; Index++) {
      Cdb[2 + Index] = (UINT8)RShiftU64 (Lba, 56 - 8 * Index);
    }

    Cdb[10] = (UINT8)(Blocks >> 24);
    Cdb[11] = (UINT8)(Blocks >> 16);
    Cdb[12] = (UINT8)(Blocks >> 8);
    Cdb[13] = (UINT8)Blocks;
  }

  //
  // Command UPIU, then an empty response UPIU and the PRDT.
  //
  for (Index = 0; Index < UCD_SIZE; Index += sizeof (UINT32)) {
    MmioWrite32 (Ucd + Index, 0);
  }

  MmioWrite32 (
    Ucd + UCD_COMMAND_OFFSET + 0x00,
    UPIU_TRANSACTION_COMMAND |
    ((UINT32)(UPIU_FLAGS_READ | UPIU_FLAGS_TASK_ATTR_SIMPLE) << 8) |
    ((UINT32)Lun << 16) |
    ((UINT32)Ufs->Slot << 24)
    );
  MmioWrite32 (Ucd + UCD_COMMAND_OFFSET + 0x04, UPIU_COMMAND_SET_SCSI);
  MmioWrite32 (Ucd + UCD_COMMAND_OFFSET + 0x08, 0);
  MmioWrite32 (Ucd + UCD_COMMAND_OFFSET + 0x0C, SwapBytes32 (Length));
  for (Index = 0; Index < sizeof (Cdb); Index += sizeof (UINT32)) {
    MmioWrite32 (
      Ucd + UCD_COMMAND_OFFSET + 0x10 + Index,
      (UINT32)Cdb[Index] |
      ((UINT32)Cdb[Index + 1] << 8) |
      ((UINT32)Cdb[Index + 2] << 16) |
      ((UINT32)Cdb[Index + 3] << 24)
      );
  }

  Entries   = 0;
  Remaining = Length;
  Address   = Buffer;
  while ((Remaining > 0) && (Entries < UCD_PRDT_MAX_ENTRIES)) {
    Chunk = MIN (Remaining, PRDT_MAX_BYTES);
    MmioWrite32 (Ucd + UCD_PRDT_OFFSET + Entries * UCD_PRDT_ENTRY_SIZE + 0x0, (UINT32)Address);
    MmioWrite32 (Ucd + UCD_PRDT_OFFSET + Entries * UCD_PRDT_ENTRY_SIZE + 0x4, (UINT32)RShiftU64 (Address, 32));
    MmioWrite32 (Ucd + UCD_PRDT_OFFSET + Entries * UCD_PRDT_ENTRY_SIZE + 0x8, 0);
    MmioWrite32 (Ucd + UCD_PRDT_OFFSET + Entries * UCD_PRDT_ENTRY_SIZE + 0xC, Chunk - 1);
    Entries++;
    Remaining -= Chunk;
    Address   += Chunk;
  }

  if (Remaining != 0) {
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_REQUEST, 0, 0, 0);
    return RETURN_DEVICE_ERROR;
  }

  //
  // The transfer request descriptor, OCS set to "invalid" so that a request
  // the controller never completed does not pass for a successful one.
  //
  MmioWrite32 (Trd + 0x00, UTRD_DW0_CT_UFS_STORAGE | UTRD_DW0_DD_DEVICE_TO_HOST | UTRD_DW0_INTERRUPT);
  MmioWrite32 (Trd + 0x04, 0);
  MmioWrite32 (Trd + 0x08, UFS_HC_TRD_OCS_INIT_VALUE);
  MmioWrite32 (Trd + 0x0C, 0);
  MmioWrite32 (Trd + 0x10, (UINT32)Ucd);
  MmioWrite32 (Trd + 0x14, (UINT32)RShiftU64 ((UINT64)Ucd, 32));
  MmioWrite32 (Trd + 0x18, ((UCD_RESPONSE_OFFSET / sizeof (UINT32)) << 16) | (UCD_RESPONSE_SIZE / sizeof (UINT32)));
  MmioWrite32 (Trd + 0x1C, ((UCD_PRDT_OFFSET / sizeof (UINT32)) << 16) | Entries);

  ArmDataSynchronizationBarrier ();

  IsBefore = MmioRead32 (Ufs->Base + UFS_HC_IS_OFFSET);
  MmioWrite32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET, Bit);

  //
  // The controller clears the doorbell bit when the request completes.
  //
  Fatal = FALSE;
  Is    = IsBefore;
  Qcs6490EarlyTimeoutStart (&Timeout, UFS_COMMAND_TIMEOUT_US);
  while ((MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET) & Bit) != 0) {
    Is = MmioRead32 (Ufs->Base + UFS_HC_IS_OFFSET);
    if ((Is & ~IsBefore & UFS_HC_IS_FATAL) != 0) {
      Fatal = TRUE;
      break;
    }

    if (Qcs6490EarlyTimeoutExpired (&Timeout)) {
      break;
    }
  }

  if (Fatal || ((MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET) & Bit) != 0)) {
    if ((MmioRead32 (Ufs->Base + UFS_HC_UTRLDBR_OFFSET) & Bit) != 0) {
      ClearSlot (Ufs);
    }

    Qcs6490Print (
      "QCS6490: UFS: READ LUN%u LBA %lu x%u %a (IS 0x%x)%a\n",
      Lun,
      Lba,
      Blocks,
      Fatal ? "failed" : "timed out",
      Is,
      Ufs->Pending ? ", not cleared" : ""
      );
    if (Fatal) {
      SetDiagnostic (Ufs, QCS6490_UFS_STEP_FATAL, 0, 0, FatalBits (Is));
      return RETURN_DEVICE_ERROR;
    }

    SetDiagnostic (Ufs, QCS6490_UFS_STEP_TIMEOUT, 0, 0, Ufs->Pending ? 1 : 0);
    return RETURN_TIMEOUT;
  }

  ArmDataSynchronizationBarrier ();

  //
  // Acknowledge what this request raised in IS.
  //
  Is    = MmioRead32 (Ufs->Base + UFS_HC_IS_OFFSET);
  Clear = Is & UFS_HC_IS_UTRCS;
  if ((Is & ~IsBefore & UFS_HC_IS_UTPES) != 0) {
    Clear |= UFS_HC_IS_UTPES;
  }

  if (Clear != 0) {
    MmioWrite32 (Ufs->Base + UFS_HC_IS_OFFSET, Clear);
  }

  Ocs = UTRD_DW2_OCS (MmioRead32 (Trd + 0x08));
  if (Ocs != UTRD_OCS_SUCCESS) {
    Qcs6490Print ("QCS6490: UFS: READ LUN%u LBA %lu x%u: OCS 0x%x (IS 0x%x)\n", Lun, Lba, Blocks, Ocs, Is);
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_OCS, Ocs, 0, 0);
    return RETURN_DEVICE_ERROR;
  }

  ResponseDw0 = MmioRead32 (Response + RESPONSE_DW0);
  ResponseDw1 = MmioRead32 (Response + RESPONSE_DW1);
  ScsiStatus  = (UINT8)(ResponseDw1 >> 24);
  if ((UPIU_TRANSACTION_CODE (ResponseDw0) != UPIU_TRANSACTION_RESPONSE) ||
      ((ResponseDw0 >> 24) != Ufs->Slot) ||
      (((ResponseDw1 >> 16) & 0xFF) != UPIU_RESPONSE_TARGET_SUCCESS))
  {
    Qcs6490Print (
      "QCS6490: UFS: READ LUN%u LBA %lu x%u: response 0x%08x 0x%08x\n",
      Lun,
      Lba,
      Blocks,
      ResponseDw0,
      ResponseDw1
      );
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_RESPONSE, Ocs, ScsiStatus, (ResponseDw1 >> 16) & 0xFF);
    return RETURN_DEVICE_ERROR;
  }

  if (ScsiStatus != SCSI_STATUS_GOOD) {
    SenseKey = (ScsiStatus == SCSI_STATUS_CHECK_CONDITION) ? GetSenseKey (Response) : EFI_SCSI_SK_NO_SENSE;
    Qcs6490Print (
      "QCS6490: UFS: READ LUN%u LBA %lu x%u: SCSI status 0x%x, sense key 0x%x\n",
      Lun,
      Lba,
      Blocks,
      ScsiStatus,
      SenseKey
      );
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_SCSI, Ocs, ScsiStatus, SenseKey);
    if ((ScsiStatus == SCSI_STATUS_BUSY) || (ScsiStatus == SCSI_STATUS_TASK_SET_FULL) ||
        ((ScsiStatus == SCSI_STATUS_CHECK_CONDITION) && (SenseKey == EFI_SCSI_SK_UNIT_ATTENTION)))
    {
      return RETURN_NOT_READY;
    }

    return RETURN_DEVICE_ERROR;
  }

  if ((((ResponseDw0 >> 8) & (UPIU_FLAGS_OVERFLOW | UPIU_FLAGS_UNDERFLOW)) != 0) &&
      (MmioRead32 (Response + RESPONSE_RESIDUAL) != 0))
  {
    Qcs6490Print (
      "QCS6490: UFS: READ LUN%u LBA %lu x%u: residual 0x%x\n",
      Lun,
      Lba,
      Blocks,
      SwapBytes32 (MmioRead32 (Response + RESPONSE_RESIDUAL))
      );
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_RESIDUAL, Ocs, ScsiStatus, 0);
    return RETURN_DEVICE_ERROR;
  }

  return RETURN_SUCCESS;
}

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
  )
{
  UINT32         Count;
  UINT32         Attempt;
  RETURN_STATUS  Status;

  if (!Ufs->Ready || Ufs->Pending) {
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_REQUEST, 0, 0, 1);
    return RETURN_NOT_READY;
  }

  //
  // UFS only ever writes into the store memory, in whole blocks.
  //
  if ((Lun > UFS_MAX_LUN) || (Blocks == 0) ||
      ((Buffer & (sizeof (UINT32) - 1)) != 0) ||
      (Blocks > QCS6490_EARLY_NV_AREA_SIZE / QCS6490_NVSTORE_BLOCK_SIZE) ||
      !Qcs6490EarlyIsInNvArea (Buffer, (UINT64)Blocks * QCS6490_NVSTORE_BLOCK_SIZE))
  {
    SetDiagnostic (Ufs, QCS6490_UFS_STEP_REQUEST, 0, 0, 2);
    return RETURN_INVALID_PARAMETER;
  }

  while (Blocks > 0) {
    Count = MIN (Blocks, READ_MAX_BLOCKS);

    Status = RETURN_NOT_READY;
    for (Attempt = 0; (Attempt < UFS_COMMAND_ATTEMPTS) && (Status == RETURN_NOT_READY); Attempt++) {
      Status = ExecuteRead (Ufs, Lun, Lba, Count, Buffer);
    }

    if (RETURN_ERROR (Status)) {
      return Status;
    }

    Ufs->Diagnostic = 0;
    Lba            += Count;
    Blocks         -= Count;
    Buffer         += (UINT64)Count * QCS6490_NVSTORE_BLOCK_SIZE;
  }

  return RETURN_SUCCESS;
}

/**
  Leaves the controller as XBL left it, as far as SEC can.

  @param[in, out] Ufs  The controller.

  @retval TRUE   No transfer is in progress: the SMMU entry may go.
  @retval FALSE  A transfer may still be in progress.
**/
BOOLEAN
Qcs6490EarlyUfsFinish (
  IN OUT QCS6490_EARLY_UFS  *Ufs
  )
{
  UINT32  Index;

  if (Ufs->Pending) {
    return FALSE;
  }

  if (Ufs->TrdSaved) {
    for (Index = 0; Index < UTRD_WORDS; Index++) {
      MmioWrite32 ((UINTN)Ufs->ListBase + Ufs->Slot * UTRD_SIZE + Index * sizeof (UINT32), Ufs->SavedTrd[Index]);
    }

    ArmDataSynchronizationBarrier ();
    Ufs->TrdSaved = FALSE;
  }

  return TRUE;
}
