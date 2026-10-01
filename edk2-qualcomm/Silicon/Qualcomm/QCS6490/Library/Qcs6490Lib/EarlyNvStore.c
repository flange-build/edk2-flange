/** @file
  The variable store as SEC sees it on QCS6490: the partition that holds it,
  the firmware volume it is kept in, and the HypervisorMode and DspPreload
  variables.

  The partition is found by name in the GPT of its LUN, so that the store
  follows the partition table rather than a block number built in. The
  store is the EDK II variable firmware volume (Guid/VariableFormat.h)
  exactly as the variable driver keeps it in memory, so SEC reads variables
  the way the variable driver does, without its code.

  This runs with the MMU off: the GPT and variable structures are packed,
  so the compiler reads their fields a byte at a time.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi/UefiBaseType.h>
#include <Uefi/UefiMultiPhase.h>
#include <Uefi/UefiGpt.h>
#include <Pi/PiFirmwareVolume.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>

#include <Guid/Qcs6490PlatformConfig.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>
#include <Qcs6490NvStore.h>

#include "Qcs6490Early.h"
#include "Qcs6490LibInternal.h"

//
// Bounds on the GPT this accepts.
//
#define GPT_MAX_ENTRIES  1024

//
// QCS6490_UFS_STEP_GPT_HEADER details.
//
#define GPT_HEADER_SIGNATURE  1
#define GPT_HEADER_SIZE       2
#define GPT_HEADER_CRC        3
#define GPT_HEADER_MY_LBA     4
#define GPT_HEADER_ENTRIES    5
#define GPT_HEADER_LBAS       6

//
// Longest firmware volume header the store may have. The block map of the
// store has two entries, so its header is 72 bytes.
//
#define NVSTORE_MAX_FV_HEADER_LENGTH  SIZE_4KB

/**
  Compares a GPT partition name with a name.

  @param[in]  Entry  The partition entry.
  @param[in]  Name   The name, NUL-terminated.

  @retval TRUE   They are the same.
  @retval FALSE  They are not.
**/
STATIC
BOOLEAN
PartitionNameMatches (
  IN CONST EFI_PARTITION_ENTRY  *Entry,
  IN CONST CHAR16               *Name
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (Entry->PartitionName); Index++) {
    if (Entry->PartitionName[Index] != Name[Index]) {
      return FALSE;
    }

    if (Name[Index] == L'\0') {
      return TRUE;
    }
  }

  //
  // A name that fills the field has no terminator in it.
  //
  return Name[Index] == L'\0';
}

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
  )
{
  EFI_PARTITION_TABLE_HEADER  *Header;
  EFI_PARTITION_ENTRY         *Entry;
  RETURN_STATUS               Status;
  UINT32                      HeaderSize;
  UINT32                      HeaderCrc;
  UINT32                      Crc;
  UINT32                      EntryCount;
  UINT32                      EntrySize;
  UINT32                      EntryBytes;
  UINT64                      EntryLba;
  UINT64                      FirstUsable;
  UINT64                      LastUsable;
  UINT64                      Start;
  UINT64                      End;
  UINT32                      Index;
  BOOLEAN                     Found;

  *StartLba   = 0;
  *BlockCount = 0;

  Header = (EFI_PARTITION_TABLE_HEADER *)(UINTN)QCS6490_EARLY_BLOCK_BUFFER_BASE;
  Status = Qcs6490EarlyUfsRead (Ufs, Lun, PRIMARY_PART_HEADER_LBA, 1, QCS6490_EARLY_BLOCK_BUFFER_BASE);
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  if (Header->Header.Signature != EFI_PTAB_HEADER_ID) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_SIGNATURE);
    return RETURN_NOT_FOUND;
  }

  HeaderSize = Header->Header.HeaderSize;
  if ((HeaderSize < sizeof (EFI_PARTITION_TABLE_HEADER)) || (HeaderSize > QCS6490_NVSTORE_BLOCK_SIZE)) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_SIZE);
    return RETURN_NOT_FOUND;
  }

  //
  // The CRC covers the header with its CRC field zeroed.
  //
  HeaderCrc            = Header->Header.CRC32;
  Header->Header.CRC32 = 0;
  Crc                  = CalculateCrc32 (Header, HeaderSize);
  Header->Header.CRC32 = HeaderCrc;
  if (Crc != HeaderCrc) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_CRC);
    return RETURN_NOT_FOUND;
  }

  if (Header->MyLBA != PRIMARY_PART_HEADER_LBA) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_MY_LBA);
    return RETURN_NOT_FOUND;
  }

  EntryCount = Header->NumberOfPartitionEntries;
  EntrySize  = Header->SizeOfPartitionEntry;
  if ((EntryCount == 0) || (EntryCount > GPT_MAX_ENTRIES) ||
      (EntrySize < sizeof (EFI_PARTITION_ENTRY)) || ((EntrySize % 8) != 0) ||
      ((UINT64)EntryCount * EntrySize > QCS6490_NVSTORE_SIZE))
  {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_ENTRIES);
    return RETURN_NOT_FOUND;
  }

  EntryLba    = Header->PartitionEntryLBA;
  FirstUsable = Header->FirstUsableLBA;
  LastUsable  = Header->LastUsableLBA;
  if ((EntryLba <= PRIMARY_PART_HEADER_LBA) || (FirstUsable > LastUsable)) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_HEADER, 0, 0, GPT_HEADER_LBAS);
    return RETURN_NOT_FOUND;
  }

  //
  // The entries go into the store memory, which the store itself replaces
  // once the partition is known.
  //
  EntryBytes = EntryCount * EntrySize;
  Status     = Qcs6490EarlyUfsRead (
                 Ufs,
                 Lun,
                 EntryLba,
                 (EntryBytes + QCS6490_NVSTORE_BLOCK_SIZE - 1) / QCS6490_NVSTORE_BLOCK_SIZE,
                 QCS6490_NVSTORE_BASE
                 );
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  if (CalculateCrc32 ((VOID *)(UINTN)QCS6490_NVSTORE_BASE, EntryBytes) != Header->PartitionEntryArrayCRC32) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_ENTRIES, 0, 0, 0);
    return RETURN_NOT_FOUND;
  }

  Found = FALSE;
  Start = 0;
  End   = 0;
  for (Index = 0; Index < EntryCount; Index++) {
    Entry = (EFI_PARTITION_ENTRY *)(UINTN)(QCS6490_NVSTORE_BASE + (UINT64)Index * EntrySize);
    if (IsZeroGuid (&Entry->PartitionTypeGUID) || !PartitionNameMatches (Entry, Name)) {
      continue;
    }

    //
    // Two partitions of that name: writing either could destroy the other.
    //
    if (Found) {
      Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_DUPLICATE, 0, 0, 0);
      return RETURN_NOT_FOUND;
    }

    Found = TRUE;
    Start = Entry->StartingLBA;
    End   = Entry->EndingLBA;
  }

  if (!Found) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_NOT_FOUND, 0, 0, 0);
    return RETURN_NOT_FOUND;
  }

  if ((Start > End) || (Start < FirstUsable) || (End > LastUsable)) {
    Ufs->Diagnostic = QCS6490_UFS_DIAG (QCS6490_UFS_STEP_GPT_RANGE, 0, 0, 0);
    return RETURN_NOT_FOUND;
  }

  *StartLba   = Start;
  *BlockCount = End - Start + 1;
  return RETURN_SUCCESS;
}

/**
  Checks that the store memory holds a variable firmware volume the
  variable driver can use: its header and the variable store header.

  @retval TRUE   It does.
  @retval FALSE  It does not.
**/
BOOLEAN
Qcs6490EarlyCheckStore (
  VOID
  )
{
  EFI_FIRMWARE_VOLUME_HEADER  *FvHeader;
  VARIABLE_STORE_HEADER       *Store;
  UINT32                      HeaderLength;

  FvHeader = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)QCS6490_NVSTORE_BASE;

  if ((FvHeader->Signature != EFI_FVH_SIGNATURE) ||
      (FvHeader->Revision != EFI_FVH_REVISION) ||
      (FvHeader->FvLength != QCS6490_NVSTORE_SIZE) ||
      !CompareGuid (&FvHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid))
  {
    return FALSE;
  }

  HeaderLength = FvHeader->HeaderLength;
  if ((HeaderLength < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) ||
      (HeaderLength > NVSTORE_MAX_FV_HEADER_LENGTH) ||
      ((HeaderLength % sizeof (UINT16)) != 0) ||
      (CalculateSum16 ((UINT16 *)FvHeader, HeaderLength) != 0))
  {
    return FALSE;
  }

  Store = (VARIABLE_STORE_HEADER *)(UINTN)(QCS6490_NVSTORE_BASE + HeaderLength);
  if ((!CompareGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid) &&
       !CompareGuid (&Store->Signature, &gEfiVariableGuid)) ||
      (Store->Size != FixedPcdGet32 (PcdFlashNvStorageVariableSize) - HeaderLength) ||
      (Store->Format != VARIABLE_STORE_FORMATTED) ||
      (Store->State != VARIABLE_STORE_HEALTHY))
  {
    return FALSE;
  }

  return TRUE;
}

/**
  Looks up a platform settings variable (vendor gQcs6490PlatformConfigGuid)
  in the store memory, which must have passed Qcs6490EarlyCheckStore ().

  Walks the variables the way the variable driver does: 4-byte aligned
  records up to the end of the store or the first one without a start
  marker. A record that is being replaced (VAR_ADDED and
  VAR_IN_DELETED_TRANSITION) counts only when no added copy exists.

  @param[in]   Name      The variable name.
  @param[in]   NameSize  Its size in bytes, the terminating null included.
  @param[out]  Data      Its value.
  @param[in]   DataSize  The size it must have.

  @retval TRUE   Found with that size; Data holds it.
  @retval FALSE  Missing, or of another size.
**/
STATIC
BOOLEAN
FindPlatformVariable (
  IN  CONST CHAR16  *Name,
  IN  UINTN         NameSize,
  OUT VOID          *Data,
  IN  UINT32        DataSize
  )
{
  EFI_FIRMWARE_VOLUME_HEADER     *FvHeader;
  VARIABLE_STORE_HEADER          *Store;
  AUTHENTICATED_VARIABLE_HEADER  *AuthVariable;
  VARIABLE_HEADER                *Variable;
  BOOLEAN                        Authenticated;
  UINTN                          HeaderSize;
  UINTN                          Current;
  UINTN                          End;
  UINTN                          NamePtr;
  UINTN                          DataPtr;
  UINT32                         RecordNameSize;
  UINT32                         RecordDataSize;
  UINTN                          NamePad;
  UINT8                          State;
  EFI_GUID                       *VendorGuid;
  UINTN                          Added;
  UINTN                          InTransition;

  FvHeader      = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)QCS6490_NVSTORE_BASE;
  Store         = (VARIABLE_STORE_HEADER *)(UINTN)(QCS6490_NVSTORE_BASE + FvHeader->HeaderLength);
  Authenticated = CompareGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid);
  HeaderSize    = Authenticated ? sizeof (AUTHENTICATED_VARIABLE_HEADER) : sizeof (VARIABLE_HEADER);

  Current      = HEADER_ALIGN ((UINTN)(Store + 1));
  End          = (UINTN)Store + Store->Size;
  Added        = 0;
  InTransition = 0;

  //
  // Every record takes at least a header, so this ends.
  //
  while ((Current < End) && (HeaderSize <= End - Current)) {
    if (Authenticated) {
      AuthVariable = (AUTHENTICATED_VARIABLE_HEADER *)Current;
      if (AuthVariable->StartId != VARIABLE_DATA) {
        break;
      }

      State          = AuthVariable->State;
      RecordNameSize = AuthVariable->NameSize;
      RecordDataSize = AuthVariable->DataSize;
      VendorGuid     = &AuthVariable->VendorGuid;
    } else {
      Variable = (VARIABLE_HEADER *)Current;
      if (Variable->StartId != VARIABLE_DATA) {
        break;
      }

      State          = Variable->State;
      RecordNameSize = Variable->NameSize;
      RecordDataSize = Variable->DataSize;
      VendorGuid     = &Variable->VendorGuid;
    }

    //
    // A record running past the end of the store ends the walk, as a
    // partly written one would.
    //
    NamePtr = Current + HeaderSize;
    NamePad = GET_PAD_SIZE (RecordNameSize);
    if ((RecordNameSize > End - NamePtr) ||
        (NamePad > End - NamePtr - RecordNameSize) ||
        (RecordDataSize > End - NamePtr - RecordNameSize - NamePad))
    {
      break;
    }

    DataPtr = NamePtr + RecordNameSize + NamePad;

    if (((State == VAR_ADDED) || (State == (VAR_ADDED & VAR_IN_DELETED_TRANSITION))) &&
        (RecordNameSize == NameSize) &&
        CompareGuid (VendorGuid, &gQcs6490PlatformConfigGuid) &&
        (CompareMem ((VOID *)NamePtr, Name, NameSize) == 0))
    {
      //
      // Remember where the value is; 1 marks a copy of the wrong size.
      //
      if (State == VAR_ADDED) {
        Added = (RecordDataSize == DataSize) ? DataPtr : 1;
      } else {
        InTransition = (RecordDataSize == DataSize) ? DataPtr : 1;
      }
    }

    Current = HEADER_ALIGN (DataPtr + RecordDataSize + GET_PAD_SIZE (RecordDataSize));
  }

  if (Added == 0) {
    Added = InTransition;
  }

  if (Added <= 1) {
    return FALSE;
  }

  CopyMem (Data, (VOID *)Added, DataSize);
  return TRUE;
}

UINT8
Qcs6490EarlyFindHypervisorMode (
  VOID
  )
{
  QCS6490_HYPERVISOR_CONFIG  Config;

  if (!FindPlatformVariable (
         QCS6490_HYPERVISOR_MODE_VARIABLE,
         sizeof (QCS6490_HYPERVISOR_MODE_VARIABLE),
         &Config,
         sizeof (Config)
         ))
  {
    return QCS6490_HYPERVISOR_SETTING_NONE;
  }

  if ((Config.Mode != QCS6490_HYPERVISOR_MODE_AUTO) &&
      (Config.Mode != QCS6490_HYPERVISOR_MODE_EL1) &&
      (Config.Mode != QCS6490_HYPERVISOR_MODE_EL2))
  {
    Qcs6490Print ("QCS6490: variables: HypervisorMode is not valid, ignored\n");
    return QCS6490_HYPERVISOR_SETTING_NONE;
  }

  return Config.Mode;
}

BOOLEAN
Qcs6490EarlyFindDspPreload (
  OUT QCS6490_DSP_PRELOAD_CONFIG  *Config
  )
{
  if (!FindPlatformVariable (
         QCS6490_DSP_PRELOAD_VARIABLE,
         sizeof (QCS6490_DSP_PRELOAD_VARIABLE),
         Config,
         sizeof (*Config)
         ))
  {
    return FALSE;
  }

  if ((Config->Mode != QCS6490_DSP_PRELOAD_AUTO) &&
      (Config->Mode != QCS6490_DSP_PRELOAD_DISABLED) &&
      (Config->Mode != QCS6490_DSP_PRELOAD_ALWAYS))
  {
    Qcs6490Print ("QCS6490: variables: DspPreload is not valid, ignored\n");
    return FALSE;
  }

  return TRUE;
}
