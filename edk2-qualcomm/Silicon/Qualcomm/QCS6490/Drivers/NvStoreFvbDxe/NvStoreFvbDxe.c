/** @file
  Firmware Volume Block driver for the QCS6490 UEFI variable store.

  The store (variables, FTW working area, FTW spare area, see
  Qcs6490NvStore.h) lives at the start of a GPT partition on UFS: "logfs" on
  LUN 4 of the RUBIK Pi 3 (PcdNvStorePartitionName, PcdNvStoreUfsLun). SEC
  reads it into memory at PcdFlashNvStorageVariableBase64 before anything else
  runs, as it needs the HypervisorMode variable to decide whether to leave
  Gunyah, and leaves a status page after it saying how that went.

  This driver serves the store from memory, as a memory-mapped FVB, to the
  variable driver and FaultTolerantWriteDxe, and writes the changes back to
  the partition:

  - If SEC loaded a valid store, memory is used as it is. Otherwise the store
    is formatted in memory: an FV header with gEfiSystemNvDataFvGuid, an empty
    authenticated variable store, the FTW areas erased.

  - Changes are written back only if SEC read the partition (it loaded the
    store, or found the partition blank), so that memory holds what is, or
    will be, on UFS. Otherwise the variables live in memory for this boot
    only: a partition SEC could not read is never overwritten.

  - Blocks changed before the partition's BlockIo shows up are remembered and
    written when it does. Later changes are written straight away: before
    Write () or EraseBlocks () returns when called at TPL_CALLBACK or below,
    otherwise as soon as the TPL drops below TPL_CALLBACK. The variable
    driver calls the FVB with its lock held at TPL_NOTIFY, where BlockIo
    cannot be used, so a SetVariable () reaches UFS when it releases the
    lock, before it returns. Anything left over is written at ReadyToBoot,
    at reset and before ExitBootServices.

  - UFS is not touched at or after ExitBootServices, when SmmuDxe takes down
    the SMMU stream that UFS DMA needs under Gunyah; the last write-back is in
    the BeforeExitBootServices group. Writes and erases fail with
    EFI_WRITE_PROTECTED at runtime, so variables set by the OS do not persist.

  FVB semantics and the store format follow RkFvbDxe in edk2-rockchip, with
  UFS BlockIo in place of SPI NOR.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

#include <Guid/EventGroup.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Guid/RtPropertiesTable.h>
#include <Protocol/ResetNotification.h>

#include "NvStoreFvbDxe.h"

//
// The regions must be contiguous, in order and made of whole blocks, as
// Qcs6490NvStore.h and the FV block map describe them.
//
STATIC_ASSERT (
  (FixedPcdGet64 (PcdFlashNvStorageVariableBase64) % QCS6490_NVSTORE_BLOCK_SIZE) == 0,
  "The variable store must start on a block boundary"
  );
STATIC_ASSERT (
  ((FixedPcdGet32 (PcdFlashNvStorageVariableSize) % QCS6490_NVSTORE_BLOCK_SIZE) == 0) &&
  ((FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize) % QCS6490_NVSTORE_BLOCK_SIZE) == 0) &&
  ((FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize) % QCS6490_NVSTORE_BLOCK_SIZE) == 0),
  "The variable, FTW working and FTW spare areas must be whole blocks"
  );
STATIC_ASSERT (
  FixedPcdGet64 (PcdFlashNvStorageFtwWorkingBase64) ==
  FixedPcdGet64 (PcdFlashNvStorageVariableBase64) + FixedPcdGet32 (PcdFlashNvStorageVariableSize),
  "The FTW working area must follow the variable area"
  );
STATIC_ASSERT (
  FixedPcdGet64 (PcdFlashNvStorageFtwSpareBase64) ==
  FixedPcdGet64 (PcdFlashNvStorageFtwWorkingBase64) + FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize),
  "The FTW spare area must follow the FTW working area"
  );
STATIC_ASSERT (
  OFFSET_OF (NVSTORE_HEADERS, VariableStore) == NVSTORE_FV_HEADER_LENGTH,
  "The variable store header must follow the FV header"
  );
STATIC_ASSERT (
  FixedPcdGet32 (PcdFlashNvStorageVariableSize) > sizeof (NVSTORE_HEADERS),
  "The variable area is too small"
  );
STATIC_ASSERT (
  sizeof (((EFI_PARTITION_ENTRY *)0)->PartitionName) == NVSTORE_GPT_NAME_LENGTH * sizeof (CHAR16),
  "Unexpected GPT partition name length"
  );

//
// QCS6490_NVSTORE_STATUS.LoadResult, for the log.
//
STATIC CONST CHAR8 *CONST  mLoadResultNames[] = {
  "not tried",           // QCS6490_NVSTORE_LOAD_NOT_TRIED
  "loaded",              // QCS6490_NVSTORE_LOAD_OK
  "blank partition",     // QCS6490_NVSTORE_LOAD_BLANK
  "no partition",        // QCS6490_NVSTORE_LOAD_NO_PARTITION
  "UFS error",           // QCS6490_NVSTORE_LOAD_UFS_ERROR
  "SMMU error",          // QCS6490_NVSTORE_LOAD_SMMU_ERROR
  "partition too small"  // QCS6490_NVSTORE_LOAD_TOO_SMALL
};

STATIC_ASSERT (
  ARRAY_SIZE (mLoadResultNames) == QCS6490_NVSTORE_LOAD_TOO_SMALL + 1,
  "A load result has no name"
  );

//
// The store: its physical address until SetVirtualAddressMap (), its virtual
// address after.
//
STATIC UINT8  *mNvStore;

//
// The FV attributes. The store is formatted with NVSTORE_FVB_ATTRIBUTES, and
// SetAttributes () may change the status bits for this boot. Kept here rather
// than read from the FV header, which FTW erases and rewrites during a
// reclaim.
//
STATIC EFI_FVB_ATTRIBUTES_2  mFvbAttributes = NVSTORE_FVB_ATTRIBUTES;

//
// What SEC reported in the status page. The load result is NOT_TRIED if the
// status page is missing or does not add up.
//
STATIC UINT32  mSecLoadResult = QCS6490_NVSTORE_LOAD_NOT_TRIED;
STATIC UINT64  mSecPartitionLba;

//
// Whether changes may be written to the partition, and if not, why not.
//
STATIC BOOLEAN      mWriteBackAllowed;
STATIC CONST CHAR8  *mWriteBackOffReason = "not decided yet";

//
// Set in the BeforeExitBootServices group, after the last write-back: UFS
// must not be used after that.
//
STATIC BOOLEAN  mUfsStopped;

//
// The partition: its name, its handle once its BlockIo has shown up (or
// whether it was seen while write-back is off), and a bounce buffer for its
// transfers if the store is not aligned enough for its IoAlign.
//
STATIC CHAR16      mPartitionName[NVSTORE_GPT_NAME_LENGTH + 1];
STATIC EFI_HANDLE  mPartitionHandle;
STATIC BOOLEAN     mPartitionSeen;
STATIC VOID        *mBounceBuffer;
STATIC UINT32      mBounceAlign;

//
// Blocks changed in memory and not yet on UFS. One byte per block rather than
// one bit, so that a block marked from a higher TPL while a write-back runs
// cannot race with a read-modify-write of a shared byte.
//
STATIC BOOLEAN  mDirty[NVSTORE_BLOCK_COUNT];
STATIC BOOLEAN  mFlushing;
STATIC UINTN    mWriteFailures;

//
// Signalled when the store changes at a TPL above TPL_CALLBACK, where BlockIo
// cannot be used; its notification function, at TPL_CALLBACK, writes the
// change back once the TPL drops. Every SetVariable () ends up here: the
// variable driver holds its lock, at TPL_NOTIFY, while it calls the FVB.
//
STATIC EFI_EVENT  mFlushEvent;

STATIC EFI_HANDLE  mFvbHandle;
STATIC VOID        *mBlockIoRegistration;
STATIC VOID        *mResetNotificationRegistration;

STATIC EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  mFvbProtocol = {
  NvStoreFvbGetAttributes,
  NvStoreFvbSetAttributes,
  NvStoreFvbGetPhysicalAddress,
  NvStoreFvbGetBlockSize,
  NvStoreFvbRead,
  NvStoreFvbWrite,
  NvStoreFvbEraseBlocks,
  NULL
};

STATIC NVSTORE_FVB_DEVICE_PATH  mFvbDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    NVSTORE_FVB_DEVICE_PATH_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

/**
  Returns the name of a SEC load result.

  @param[in]  LoadResult  A QCS6490_NVSTORE_LOAD_* value.

  @return  Its name.
**/
STATIC
CONST CHAR8 *
NvStoreLoadResultName (
  IN UINT32  LoadResult
  )
{
  if (LoadResult < ARRAY_SIZE (mLoadResultNames)) {
    return mLoadResultNames[LoadResult];
  }

  return "unknown result";
}

/**
  Returns the number of blocks changed in memory and not yet on UFS.

  @return  The number of blocks.
**/
STATIC
UINTN
NvStoreDirtyCount (
  VOID
  )
{
  UINTN  Lba;
  UINTN  Count;

  Count = 0;
  for (Lba = 0; Lba < NVSTORE_BLOCK_COUNT; Lba++) {
    if (mDirty[Lba]) {
      Count++;
    }
  }

  return Count;
}

/**
  Raises the TPL to TPL_CALLBACK if it is lower, so that the store and the
  dirty map are not changed under the feet of this driver's own events.

  @return  The TPL on entry, for NvStoreLeave ().
**/
STATIC
EFI_TPL
NvStoreEnter (
  VOID
  )
{
  EFI_TPL  Tpl;

  Tpl = EfiGetCurrentTpl ();
  if (Tpl < TPL_CALLBACK) {
    gBS->RaiseTPL (TPL_CALLBACK);
  }

  return Tpl;
}

/**
  Goes back to the TPL NvStoreEnter () was called at.

  @param[in]  Tpl  What NvStoreEnter () returned.
**/
STATIC
VOID
NvStoreLeave (
  IN EFI_TPL  Tpl
  )
{
  if (Tpl < TPL_CALLBACK) {
    gBS->RestoreTPL (Tpl);
  }
}

/**
  Returns the UFS LUN a handle's device path goes through.

  @param[in]  Handle  The handle.
  @param[out] Lun     The LUN of its UFS device path node.

  @retval TRUE   The device path has a UFS node.
  @retval FALSE  It has none, or the handle has no device path.
**/
STATIC
BOOLEAN
NvStoreGetUfsLun (
  IN  EFI_HANDLE  Handle,
  OUT UINT8       *Lun
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINTN                     Length;
  UINTN                     Index;

  Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Node);
  if (EFI_ERROR (Status) || (Node == NULL)) {
    return FALSE;
  }

  for (Index = 0; Index < NVSTORE_MAX_DEVICE_PATH_NODES; Index++) {
    if (Node->Type == END_DEVICE_PATH_TYPE) {
      return FALSE;
    }

    Length = Node->Length[0] | ((UINTN)Node->Length[1] << 8);
    if (Length < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
      return FALSE;
    }

    if ((Node->Type == MESSAGING_DEVICE_PATH) &&
        (Node->SubType == MSG_UFS_DP) &&
        (Length >= sizeof (UFS_DEVICE_PATH)))
    {
      *Lun = ((UFS_DEVICE_PATH *)Node)->Lun;
      return TRUE;
    }

    Node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Node + Length);
  }

  return FALSE;
}

/**
  Returns whether a GPT partition has the name PcdNvStorePartitionName.

  @param[in]  PartitionInfo  The partition.

  @retval TRUE   The names match.
  @retval FALSE  They do not.
**/
STATIC
BOOLEAN
NvStoreNameMatches (
  IN CONST EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo
  )
{
  UINTN   Index;
  CHAR16  Char;

  //
  // Character by character: the name is a member of a packed structure, and
  // need not be aligned for the string functions.
  //
  for (Index = 0; Index < NVSTORE_GPT_NAME_LENGTH; Index++) {
    Char = PartitionInfo->Info.Gpt.PartitionName[Index];
    if (Char != mPartitionName[Index]) {
      return FALSE;
    }

    if (Char == L'\0') {
      return TRUE;
    }
  }

  return TRUE;
}

/**
  Checks whether a handle is the partition that holds the store, and whether
  the store can be written to it.

  It must be a GPT partition named PcdNvStorePartitionName on UFS LUN
  PcdNvStoreUfsLun, big enough, with 4 KiB blocks, writable, and, when SEC
  read the store, where SEC read it from.

  @param[in]  Handle    The handle.
  @param[in]  Verbose   Whether to log why a partition with the right name
                        cannot be used.
  @param[out] StartLba  The first block of the partition on the LUN, when it
                        has the right name.

  @return  Its BlockIo, or NULL if the store cannot be written to it.
**/
STATIC
EFI_BLOCK_IO_PROTOCOL *
NvStoreMatchPartition (
  IN  EFI_HANDLE  Handle,
  IN  BOOLEAN     Verbose,
  OUT EFI_LBA     *StartLba
  )
{
  EFI_STATUS                   Status;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  EFI_BLOCK_IO_MEDIA           *Media;
  UINT8                        Lun;
  CONST CHAR8                  *Problem;

  Status = gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&PartitionInfo);
  if (EFI_ERROR (Status) ||
      (PartitionInfo == NULL) ||
      (PartitionInfo->Type != PARTITION_TYPE_GPT) ||
      !NvStoreNameMatches (PartitionInfo))
  {
    return NULL;
  }

  *StartLba = PartitionInfo->Info.Gpt.StartingLBA;

  if (!NvStoreGetUfsLun (Handle, &Lun)) {
    if (Verbose) {
      DEBUG ((DEBUG_INFO, "%a: %s at LBA %lu is not on UFS, ignored\n", __func__, mPartitionName, *StartLba));
    }

    return NULL;
  }

  if (Lun != FixedPcdGet8 (PcdNvStoreUfsLun)) {
    if (Verbose) {
      DEBUG ((
        DEBUG_INFO,
        "%a: %s on UFS LUN %u ignored, the store is on LUN %u\n",
        __func__,
        mPartitionName,
        Lun,
        FixedPcdGet8 (PcdNvStoreUfsLun)
        ));
    }

    return NULL;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
  if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
    return NULL;
  }

  Media = BlockIo->Media;
  if (!Media->MediaPresent) {
    Problem = "no media";
  } else if (Media->ReadOnly) {
    Problem = "read-only";
  } else if (!Media->LogicalPartition) {
    Problem = "not a partition";
  } else if (Media->BlockSize != QCS6490_NVSTORE_BLOCK_SIZE) {
    Problem = "its blocks are not 4 KiB";
  } else if (Media->LastBlock < NVSTORE_BLOCK_COUNT - 1) {
    Problem = "smaller than the store";
  } else if ((Media->IoAlign > 1) && ((Media->IoAlign & (Media->IoAlign - 1)) != 0)) {
    Problem = "IoAlign is not a power of two";
  } else if (((mSecLoadResult == QCS6490_NVSTORE_LOAD_OK) ||
              (mSecLoadResult == QCS6490_NVSTORE_LOAD_BLANK)) &&
             (*StartLba != mSecPartitionLba))
  {
    Problem = "SEC read the store from another LBA";
  } else {
    return BlockIo;
  }

  if (Verbose) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %s on UFS LUN %u at LBA %lu cannot hold the store: %a\n",
      __func__,
      mPartitionName,
      Lun,
      *StartLba,
      Problem
      ));
  }

  return NULL;
}

/**
  Sets up the transfers to a partition: in place when every block of the
  store is aligned enough for its IoAlign, through a bounce buffer otherwise.

  @param[in]  BlockIo  The partition's BlockIo.

  @retval TRUE   Blocks can be written.
  @retval FALSE  No bounce buffer could be allocated.
**/
STATIC
BOOLEAN
NvStorePrepareIo (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  UINT32  IoAlign;

  IoAlign = BlockIo->Media->IoAlign;
  if ((IoAlign <= 1) || ((((UINTN)mNvStore | QCS6490_NVSTORE_BLOCK_SIZE) & (IoAlign - 1)) == 0)) {
    return TRUE;
  }

  if ((mBounceBuffer != NULL) && (mBounceAlign >= IoAlign)) {
    return TRUE;
  }

  if (mBounceBuffer != NULL) {
    FreeAlignedPages (mBounceBuffer, EFI_SIZE_TO_PAGES (QCS6490_NVSTORE_BLOCK_SIZE));
    mBounceBuffer = NULL;
  }

  mBounceBuffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (QCS6490_NVSTORE_BLOCK_SIZE), IoAlign);
  if (mBounceBuffer == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: no bounce buffer for IoAlign %u\n", __func__, IoAlign));
    return FALSE;
  }

  mBounceAlign = IoAlign;
  DEBUG ((DEBUG_INFO, "%a: IoAlign %u, writing through a bounce buffer\n", __func__, IoAlign));
  return TRUE;
}

/**
  Writes consecutive blocks of the store to the partition.

  @param[in]  BlockIo  The partition's BlockIo.
  @param[in]  Lba      The first block, in the store and in the partition.
  @param[in]  Count    The number of blocks.

  @retval EFI_SUCCESS  The blocks were written.
  @retval Other        BlockIo failed.
**/
STATIC
EFI_STATUS
NvStoreWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINTN                  Lba,
  IN UINTN                  Count
  )
{
  EFI_STATUS  Status;
  UINT8       *Data;
  UINTN       Index;

  Data = mNvStore + Lba * QCS6490_NVSTORE_BLOCK_SIZE;

  if (mBounceBuffer == NULL) {
    return BlockIo->WriteBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      Lba,
                      Count * QCS6490_NVSTORE_BLOCK_SIZE,
                      Data
                      );
  }

  for (Index = 0; Index < Count; Index++) {
    CopyMem (mBounceBuffer, Data + Index * QCS6490_NVSTORE_BLOCK_SIZE, QCS6490_NVSTORE_BLOCK_SIZE);
    Status = BlockIo->WriteBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        Lba + Index,
                        QCS6490_NVSTORE_BLOCK_SIZE,
                        mBounceBuffer
                        );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Writes the changed blocks to the partition and flushes it. Runs at
  TPL_CALLBACK.

  A block stays marked if it could not be written, to be tried again at the
  next write-back. After NVSTORE_MAX_WRITE_FAILURES failed write-backs in a
  row, write-back is turned off for the rest of the boot.

  @param[out] Written  The number of blocks written, optional.

  @retval EFI_SUCCESS    Every changed block is on UFS.
  @retval EFI_NOT_READY  Write-back is off, UFS is stopped, or the partition
                         is not known (any more).
  @retval Other          UFS failed.
**/
STATIC
EFI_STATUS
NvStoreFlush (
  OUT UINTN  *Written OPTIONAL
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_LBA                StartLba;
  BOOLEAN                Pending[NVSTORE_BLOCK_COUNT];
  UINTN                  Lba;
  UINTN                  Count;
  UINTN                  Total;

  if (Written != NULL) {
    *Written = 0;
  }

  if (!mWriteBackAllowed || mUfsStopped || mFlushing || (mPartitionHandle == NULL)) {
    return EFI_NOT_READY;
  }

  if (NvStoreDirtyCount () == 0) {
    return EFI_SUCCESS;
  }

  //
  // Check the partition again: its handle goes away if UFS is disconnected,
  // and must not be used then. A new one is picked up by NvStoreOnBlockIo ().
  //
  BlockIo = NvStoreMatchPartition (mPartitionHandle, FALSE, &StartLba);
  if (BlockIo == NULL) {
    DEBUG ((DEBUG_WARN, "%a: %s has gone away, changes stay in memory until it is back\n", __func__, mPartitionName));
    mPartitionHandle = NULL;
    return EFI_NOT_READY;
  }

  mFlushing = TRUE;
  ZeroMem (Pending, sizeof (Pending));
  Status = EFI_SUCCESS;
  Total  = 0;

  for (Lba = 0; Lba < NVSTORE_BLOCK_COUNT; Lba += Count) {
    if (!mDirty[Lba]) {
      Count = 1;
      continue;
    }

    //
    // A run of changed blocks. They are unmarked before the transfer, so that
    // a change made meanwhile, from a higher TPL, marks its block again.
    //
    for (Count = 0; (Lba + Count < NVSTORE_BLOCK_COUNT) && mDirty[Lba + Count]; Count++) {
      mDirty[Lba + Count]  = FALSE;
      Pending[Lba + Count] = TRUE;
    }

    Status = NvStoreWriteBlocks (BlockIo, Lba, Count);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: writing blocks %u-%u of the store to %s: %r\n",
        __func__,
        (UINT32)Lba,
        (UINT32)(Lba + Count - 1),
        mPartitionName,
        Status
        ));
      break;
    }

    Total += Count;
  }

  if (!EFI_ERROR (Status)) {
    Status = BlockIo->FlushBlocks (BlockIo);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: flushing %s: %r\n", __func__, mPartitionName, Status));
    }
  }

  if (EFI_ERROR (Status)) {
    for (Lba = 0; Lba < NVSTORE_BLOCK_COUNT; Lba++) {
      if (Pending[Lba]) {
        mDirty[Lba] = TRUE;
      }
    }

    mWriteFailures++;
    if (mWriteFailures >= NVSTORE_MAX_WRITE_FAILURES) {
      mWriteBackAllowed   = FALSE;
      mWriteBackOffReason = "UFS writes kept failing";
      DEBUG ((
        DEBUG_ERROR,
        "%a: %u write-backs failed in a row, variable changes stay in memory for the rest of this boot\n",
        __func__,
        (UINT32)mWriteFailures
        ));
    }
  } else {
    mWriteFailures = 0;
    DEBUG ((DEBUG_VERBOSE, "%a: wrote %u blocks to %s\n", __func__, (UINT32)Total, mPartitionName));
  }

  if (Written != NULL) {
    *Written = Total;
  }

  mFlushing = FALSE;
  return Status;
}

/**
  Writes the changed blocks back from an event or a notification, if the TPL
  allows BlockIo.

  @param[out] Written  The number of blocks written, optional.

  @retval EFI_SUCCESS    Every changed block is on UFS.
  @retval EFI_NOT_READY  Nothing could be written now.
  @retval Other          UFS failed.
**/
STATIC
EFI_STATUS
NvStoreSync (
  OUT UINTN  *Written OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_TPL     Tpl;

  if (Written != NULL) {
    *Written = 0;
  }

  Tpl = NvStoreEnter ();
  if (Tpl > TPL_CALLBACK) {
    Status = EFI_NOT_READY;
  } else {
    Status = NvStoreFlush (Written);
  }

  NvStoreLeave (Tpl);
  return Status;
}

/**
  Writes back the changes made at a TPL too high for BlockIo, now that the
  TPL has dropped below TPL_CALLBACK.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnFlushEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  NvStoreSync (NULL);
}

/**
  Writes a change to the store back to UFS, right after Write () or
  EraseBlocks () changed memory, when write-back is on and possible.

  BlockIo may only be used up to TPL_CALLBACK. A change made at a higher TPL,
  which is where the variable driver makes all of them, stays marked and is
  written by NvStoreOnFlushEvent () as soon as the TPL drops below
  TPL_CALLBACK.

  @param[in]  CallerTpl  The TPL Write () or EraseBlocks () was called at.
**/
STATIC
VOID
NvStoreCommit (
  IN EFI_TPL  CallerTpl
  )
{
  if (!mWriteBackAllowed) {
    return;
  }

  if (mUfsStopped) {
    DEBUG ((DEBUG_WARN, "%a: variable store changed after the last write-back, the change is not saved\n", __func__));
    return;
  }

  if (CallerTpl <= TPL_CALLBACK) {
    NvStoreFlush (NULL);
  } else if ((mFlushEvent != NULL) && (mPartitionHandle != NULL)) {
    gBS->SignalEvent (mFlushEvent);
  }
}

/**
  Returns whether the partition's handle is known and still is the
  partition. A handle that has gone away, as when UFS is disconnected and
  connected again, is forgotten, so that the new one can be picked up.

  @retval TRUE   mPartitionHandle is the partition.
  @retval FALSE  The partition is not known (any more).
**/
STATIC
BOOLEAN
NvStorePartitionKnown (
  VOID
  )
{
  EFI_LBA  StartLba;

  if (mPartitionHandle == NULL) {
    return FALSE;
  }

  if (NvStoreMatchPartition (mPartitionHandle, FALSE, &StartLba) != NULL) {
    return TRUE;
  }

  DEBUG ((DEBUG_WARN, "%a: %s has gone away, looking for it again\n", __func__, mPartitionName));
  mPartitionHandle = NULL;
  return FALSE;
}

/**
  Takes a handle that may be the partition: if it is, writes the changed
  blocks to it and uses it from then on.

  @param[in]  Handle  A handle with BlockIo.
**/
STATIC
VOID
NvStoreProbeHandle (
  IN EFI_HANDLE  Handle
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_LBA                StartLba;
  UINTN                  Written;

  BlockIo = NvStoreMatchPartition (Handle, TRUE, &StartLba);
  if (BlockIo == NULL) {
    return;
  }

  if (!mWriteBackAllowed) {
    mPartitionSeen = TRUE;
    DEBUG ((
      DEBUG_WARN,
      "%a: found %s on UFS LUN %u at LBA %lu, not writing to it: %a\n",
      __func__,
      mPartitionName,
      FixedPcdGet8 (PcdNvStoreUfsLun),
      StartLba,
      mWriteBackOffReason
      ));
    return;
  }

  if (!NvStorePrepareIo (BlockIo)) {
    return;
  }

  mPartitionHandle = Handle;
  DEBUG ((
    DEBUG_INFO,
    "%a: found %s on UFS LUN %u at LBA %lu (%lu blocks), %u changed blocks to write\n",
    __func__,
    mPartitionName,
    FixedPcdGet8 (PcdNvStoreUfsLun),
    StartLba,
    BlockIo->Media->LastBlock + 1,
    (UINT32)NvStoreDirtyCount ()
    ));

  Status = NvStoreSync (&Written);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: variable store on UFS up to date, %u blocks written\n", __func__, (UINT32)Written));
  }
}

/**
  Looks for the partition among newly installed BlockIo instances.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnBlockIo (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       HandleSize;

  //
  // Each call returns the next handle installed since the last one, until
  // there is none left.
  //
  while (TRUE) {
    HandleSize = sizeof (Handle);
    Status     = gBS->LocateHandle (
                        ByRegisterNotify,
                        NULL,
                        mBlockIoRegistration,
                        &HandleSize,
                        &Handle
                        );
    if (EFI_ERROR (Status)) {
      return;
    }

    //
    // A partition handle still held may be stale: after UFS is reconnected
    // the new handle comes through here, and must not be passed over.
    //
    if (!mPartitionSeen && !NvStorePartitionKnown ()) {
      NvStoreProbeHandle (Handle);
    }
  }
}

/**
  Looks for the partition among the BlockIo instances already installed.
**/
STATIC
VOID
NvStoreFindPartition (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;
  EFI_TPL     Tpl;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  Tpl = NvStoreEnter ();
  for (Index = 0; (Index < HandleCount) && (mPartitionHandle == NULL) && !mPartitionSeen; Index++) {
    NvStoreProbeHandle (Handles[Index]);
  }

  NvStoreLeave (Tpl);
  FreePool (Handles);
}

/**
  Writes what is left before booting.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  Dirty;

  if (!mWriteBackAllowed) {
    return;
  }

  //
  // Look once more if the partition is not known: its BlockIo notification
  // may have come while a stale handle still checked out, or while no bounce
  // buffer could be had.
  //
  if (!NvStorePartitionKnown ()) {
    NvStoreFindPartition ();
  }

  if (mPartitionHandle == NULL) {
    DEBUG ((
      DEBUG_WARN,
      "%a: %s on UFS LUN %u not available, %u changed blocks of the variable store are not saved\n",
      __func__,
      mPartitionName,
      FixedPcdGet8 (PcdNvStoreUfsLun),
      (UINT32)NvStoreDirtyCount ()
      ));
    return;
  }

  NvStoreSync (NULL);
  Dirty = NvStoreDirtyCount ();
  if (Dirty != 0) {
    DEBUG ((DEBUG_ERROR, "%a: %u changed blocks could not be written to %s\n", __func__, (UINT32)Dirty, mPartitionName));
  }
}

/**
  Writes what is left, for the last time, and stops using UFS: at
  ExitBootServices SmmuDxe takes down the SMMU stream UFS DMA goes through
  under Gunyah, and a transfer after that resets the board.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  Dirty;

  if (mUfsStopped) {
    return;
  }

  if (mWriteBackAllowed) {
    NvStoreSync (NULL);
    Dirty = NvStoreDirtyCount ();
    if (Dirty != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: %u changed blocks of the variable store were not saved\n",
        __func__,
        (UINT32)Dirty
        ));
    }
  }

  mUfsStopped = TRUE;
}

/**
  Writes what is left before a reset at boot time.

  @param[in]  ResetType    The type of reset.
  @param[in]  ResetStatus  The reason for it.
  @param[in]  DataSize     The size of ResetData.
  @param[in]  ResetData    Reset data, optional.
**/
STATIC
VOID
EFIAPI
NvStoreOnReset (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  if (EfiAtRuntime () || mUfsStopped || !mWriteBackAllowed || (NvStoreDirtyCount () == 0)) {
    return;
  }

  NvStoreSync (NULL);
}

/**
  Registers NvStoreOnReset () once the reset notification protocol is there.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnResetNotificationInstalled (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotification;

  Status = gBS->LocateProtocol (&gEfiResetNotificationProtocolGuid, NULL, (VOID **)&ResetNotification);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = ResetNotification->RegisterResetNotify (ResetNotification, NvStoreOnReset);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no write-back at reset: %r\n", __func__, Status));
  }

  gBS->CloseEvent (Event);
}

/**
  Converts the pointers used at runtime to virtual addresses.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
NvStoreOnVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0x0, (VOID **)&mNvStore);

  //
  // The variable driver calls the FVB at runtime through these. It converts
  // them itself only if they were left alone.
  //
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetAttributes);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.SetAttributes);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetPhysicalAddress);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetBlockSize);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.Read);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.Write);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.EraseBlocks);
}

/**
  Checks that PEI reserved the store and the status page for the runtime, as
  EfiRuntimeServicesData. If not, what is in there cannot be trusted; the
  range is claimed now if it is free.

  @retval TRUE   The range is runtime services data, or the memory map could
                 not be read.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
NvStoreCheckMemory (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Attempt;
  UINTN                  Offset;
  EFI_PHYSICAL_ADDRESS   Start;
  EFI_PHYSICAL_ADDRESS   End;
  EFI_PHYSICAL_ADDRESS   EntryStart;
  EFI_PHYSICAL_ADDRESS   EntryEnd;
  EFI_PHYSICAL_ADDRESS   Address;
  UINT64                 Covered;
  BOOLEAN                Reserved;

  Start = QCS6490_NVSTORE_BASE;
  End   = Start + QCS6490_NVSTORE_RESERVED_SIZE;

  Map            = NULL;
  MapSize        = 0;
  DescriptorSize = sizeof (EFI_MEMORY_DESCRIPTOR);
  Status         = EFI_BUFFER_TOO_SMALL;
  for (Attempt = 0; (Attempt < 4) && (Status == EFI_BUFFER_TOO_SMALL); Attempt++) {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (Map != NULL) {
        FreePool (Map);
      }

      //
      // Room for the descriptors the allocation itself may add.
      //
      MapSize += 8 * MAX (DescriptorSize, sizeof (EFI_MEMORY_DESCRIPTOR));
      Map      = AllocatePool (MapSize);
      if (Map == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
      }
    }
  }

  if (EFI_ERROR (Status) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    DEBUG ((DEBUG_WARN, "%a: cannot read the memory map (%r), assuming the store is reserved\n", __func__, Status));
    if (Map != NULL) {
      FreePool (Map);
    }

    return TRUE;
  }

  Covered  = 0;
  Reserved = TRUE;
  for (Offset = 0; Offset + DescriptorSize <= MapSize; Offset += DescriptorSize) {
    Entry      = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    EntryStart = Entry->PhysicalStart;
    EntryEnd   = EntryStart + Entry->NumberOfPages * EFI_PAGE_SIZE;
    if ((EntryEnd <= Start) || (EntryStart >= End)) {
      continue;
    }

    if (Entry->Type != EfiRuntimeServicesData) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: 0x%lx-0x%lx of the store is memory type %u, not runtime services data\n",
        __func__,
        MAX (EntryStart, Start),
        MIN (EntryEnd, End) - 1,
        Entry->Type
        ));
      Reserved = FALSE;
      break;
    }

    Covered += MIN (EntryEnd, End) - MAX (EntryStart, Start);
  }

  FreePool (Map);

  if (Reserved && (Covered == End - Start)) {
    return TRUE;
  }

  Address = Start;
  Status  = gBS->AllocatePages (
                   AllocateAddress,
                   EfiRuntimeServicesData,
                   EFI_SIZE_TO_PAGES (QCS6490_NVSTORE_RESERVED_SIZE),
                   &Address
                   );
  DEBUG ((
    DEBUG_ERROR,
    "%a: PEI did not reserve 0x%lx-0x%lx for the store as runtime services data (%a), its content is not trusted\n",
    __func__,
    Start,
    End - 1,
    EFI_ERROR (Status) ? "and it is in use" : "reserved now"
    ));
  return FALSE;
}

/**
  Reads the status page SEC left after the store. If it is missing or does
  not add up, the store counts as not loaded.
**/
STATIC
VOID
NvStoreReadStatus (
  VOID
  )
{
  CONST QCS6490_NVSTORE_STATUS  *Status;

  Status = (CONST QCS6490_NVSTORE_STATUS *)(UINTN)QCS6490_NVSTORE_STATUS_BASE;

  if ((Status->Signature != QCS6490_NVSTORE_STATUS_SIGNATURE) ||
      (Status->Version != QCS6490_NVSTORE_STATUS_VERSION) ||
      (Status->Size != sizeof (QCS6490_NVSTORE_STATUS)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no status from SEC at 0x%lx (signature 0x%x, version %u, size %u)\n",
      __func__,
      (UINT64)QCS6490_NVSTORE_STATUS_BASE,
      Status->Signature,
      Status->Version,
      Status->Size
      ));
    mSecLoadResult = QCS6490_NVSTORE_LOAD_NOT_TRIED;
    return;
  }

  mSecLoadResult   = Status->LoadResult;
  mSecPartitionLba = Status->PartitionLba;

  DEBUG ((
    DEBUG_INFO,
    "%a: SEC: %a (UFS diagnostic 0x%x), LUN %u, partition LBA %lu, %lu blocks of %u bytes\n",
    __func__,
    NvStoreLoadResultName (Status->LoadResult),
    Status->UfsDiagnostic,
    Status->Lun,
    Status->PartitionLba,
    Status->PartitionBlocks,
    Status->BlockSize
    ));

  //
  // Memory can only stand for the partition if SEC read the partition this
  // driver writes to.
  //
  if (((mSecLoadResult == QCS6490_NVSTORE_LOAD_OK) ||
       (mSecLoadResult == QCS6490_NVSTORE_LOAD_BLANK)) &&
      ((Status->Lun != FixedPcdGet8 (PcdNvStoreUfsLun)) ||
       (Status->BlockSize != QCS6490_NVSTORE_BLOCK_SIZE) ||
       (Status->PartitionLba == 0) ||
       (Status->PartitionBlocks < NVSTORE_BLOCK_COUNT)))
  {
    DEBUG ((DEBUG_ERROR, "%a: SEC status does not match the store's LUN, block size or size, ignored\n", __func__));
    mSecLoadResult = QCS6490_NVSTORE_LOAD_NOT_TRIED;
  }
}

/**
  Checks the FV header and the variable store header in memory against the
  format NvStoreFormat () writes.

  @return  NULL if they are valid, what is wrong otherwise.
**/
STATIC
CONST CHAR8 *
NvStoreCheckHeaders (
  VOID
  )
{
  NVSTORE_HEADERS  *Headers;

  Headers = (NVSTORE_HEADERS *)mNvStore;

  if (Headers->FvHeader.Signature != EFI_FVH_SIGNATURE) {
    return "no FV header";
  }

  if (Headers->FvHeader.Revision != EFI_FVH_REVISION) {
    return "unknown FV header revision";
  }

  if (!CompareGuid (&Headers->FvHeader.FileSystemGuid, &gEfiSystemNvDataFvGuid)) {
    return "not a system NV data FV";
  }

  if (Headers->FvHeader.FvLength != QCS6490_NVSTORE_SIZE) {
    return "wrong FV length";
  }

  if (Headers->FvHeader.HeaderLength != NVSTORE_FV_HEADER_LENGTH) {
    return "wrong FV header length";
  }

  if (CalculateSum16 ((UINT16 *)&Headers->FvHeader, NVSTORE_FV_HEADER_LENGTH) != 0) {
    return "bad FV header checksum";
  }

  if (Headers->FvHeader.Attributes != NVSTORE_FVB_ATTRIBUTES) {
    return "unexpected FV attributes";
  }

  if ((Headers->FvHeader.BlockMap[0].NumBlocks != NVSTORE_BLOCK_COUNT) ||
      (Headers->FvHeader.BlockMap[0].Length != QCS6490_NVSTORE_BLOCK_SIZE) ||
      (Headers->BlockMapEnd.NumBlocks != 0) ||
      (Headers->BlockMapEnd.Length != 0))
  {
    return "wrong FV block map";
  }

  if (!CompareGuid (&Headers->VariableStore.Signature, &gEfiAuthenticatedVariableGuid)) {
    return "not an authenticated variable store";
  }

  if (Headers->VariableStore.Size != NVSTORE_VARIABLE_STORE_SIZE) {
    return "wrong variable store size";
  }

  if ((Headers->VariableStore.Format != VARIABLE_STORE_FORMATTED) ||
      (Headers->VariableStore.State != VARIABLE_STORE_HEALTHY))
  {
    return "variable store not formatted or not healthy";
  }

  return NULL;
}

/**
  Formats the store in memory: everything erased to 0xFF, then an FV header
  covering the variable, FTW working and FTW spare areas, and an empty
  authenticated variable store. Every block is marked changed.
**/
STATIC
VOID
NvStoreFormat (
  VOID
  )
{
  NVSTORE_HEADERS  *Headers;

  Headers = (NVSTORE_HEADERS *)mNvStore;

  SetMem (Headers, QCS6490_NVSTORE_SIZE, 0xFF);
  ZeroMem (Headers, sizeof (*Headers));

  CopyGuid (&Headers->FvHeader.FileSystemGuid, &gEfiSystemNvDataFvGuid);
  Headers->FvHeader.FvLength               = QCS6490_NVSTORE_SIZE;
  Headers->FvHeader.Signature              = EFI_FVH_SIGNATURE;
  Headers->FvHeader.Attributes             = NVSTORE_FVB_ATTRIBUTES;
  Headers->FvHeader.HeaderLength           = NVSTORE_FV_HEADER_LENGTH;
  Headers->FvHeader.Revision               = EFI_FVH_REVISION;
  Headers->FvHeader.BlockMap[0].NumBlocks  = NVSTORE_BLOCK_COUNT;
  Headers->FvHeader.BlockMap[0].Length     = QCS6490_NVSTORE_BLOCK_SIZE;
  Headers->FvHeader.Checksum               = CalculateCheckSum16 (
                                               (UINT16 *)&Headers->FvHeader,
                                               NVSTORE_FV_HEADER_LENGTH
                                               );

  CopyGuid (&Headers->VariableStore.Signature, &gEfiAuthenticatedVariableGuid);
  Headers->VariableStore.Size   = NVSTORE_VARIABLE_STORE_SIZE;
  Headers->VariableStore.Format = VARIABLE_STORE_FORMATTED;
  Headers->VariableStore.State  = VARIABLE_STORE_HEALTHY;

  SetMem (mDirty, sizeof (mDirty), TRUE);
}

/**
  Copies PcdNvStorePartitionName, a UTF-16 string, into mPartitionName,
  cut to the length of a GPT partition name.
**/
STATIC
VOID
NvStoreReadPartitionName (
  VOID
  )
{
  UINTN  Size;

  Size = FixedPcdGetSize (PcdNvStorePartitionName);
  if (Size > NVSTORE_GPT_NAME_LENGTH * sizeof (CHAR16)) {
    Size = NVSTORE_GPT_NAME_LENGTH * sizeof (CHAR16);
  }

  CopyMem (mPartitionName, FixedPcdGetPtr (PcdNvStorePartitionName), Size);
  mPartitionName[NVSTORE_GPT_NAME_LENGTH] = L'\0';
}

/**
  Returns the attributes of the variable store FV.

  @param[in]  This        The protocol instance.
  @param[out] Attributes  The attributes and their current settings.

  @retval EFI_SUCCESS            They were returned.
  @retval EFI_INVALID_PARAMETER  Attributes is NULL.
**/
EFI_STATUS
EFIAPI
NvStoreFvbGetAttributes (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT       EFI_FVB_ATTRIBUTES_2                 *Attributes
  )
{
  if (Attributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Attributes = mFvbAttributes;
  return EFI_SUCCESS;
}

/**
  Changes the status bits of the variable store FV attributes for this boot.
  The FV header in the store is left as it is.

  @param[in]      This        The protocol instance.
  @param[in, out] Attributes  On input the attributes wanted, on output the
                              attributes now in effect.

  @retval EFI_SUCCESS            The attributes were changed.
  @retval EFI_INVALID_PARAMETER  They conflict with the FV's capabilities.
  @retval EFI_ACCESS_DENIED      The FV is locked.
**/
EFI_STATUS
EFIAPI
NvStoreFvbSetAttributes (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN OUT    EFI_FVB_ATTRIBUTES_2                 *Attributes
  )
{
  EFI_FVB_ATTRIBUTES_2  Capabilities;
  EFI_FVB_ATTRIBUTES_2  OldStatus;
  EFI_FVB_ATTRIBUTES_2  NewStatus;

  if (Attributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Only the status bits can change.
  //
  if (((*Attributes ^ mFvbAttributes) & ~(EFI_FVB_ATTRIBUTES_2)NVSTORE_FVB2_STATUS) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Capabilities = mFvbAttributes & NVSTORE_FVB2_CAPABILITIES;
  OldStatus    = mFvbAttributes & NVSTORE_FVB2_STATUS;
  NewStatus    = *Attributes & NVSTORE_FVB2_STATUS;

  if (((OldStatus & EFI_FVB2_LOCK_STATUS) != 0) && (NewStatus != OldStatus)) {
    return EFI_ACCESS_DENIED;
  }

  //
  // A status bit can be cleared only with the matching DISABLED capability,
  // and set only with the matching ENABLED capability.
  //
  if ((((NewStatus & EFI_FVB2_READ_STATUS) == 0) && ((Capabilities & EFI_FVB2_READ_DISABLED_CAP) == 0)) ||
      (((NewStatus & EFI_FVB2_READ_STATUS) != 0) && ((Capabilities & EFI_FVB2_READ_ENABLED_CAP) == 0)) ||
      (((NewStatus & EFI_FVB2_WRITE_STATUS) == 0) && ((Capabilities & EFI_FVB2_WRITE_DISABLED_CAP) == 0)) ||
      (((NewStatus & EFI_FVB2_WRITE_STATUS) != 0) && ((Capabilities & EFI_FVB2_WRITE_ENABLED_CAP) == 0)) ||
      (((NewStatus & EFI_FVB2_LOCK_STATUS) != 0) && ((Capabilities & EFI_FVB2_LOCK_CAP) == 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  mFvbAttributes = (mFvbAttributes & ~(EFI_FVB_ATTRIBUTES_2)NVSTORE_FVB2_STATUS) | NewStatus;
  *Attributes    = mFvbAttributes;
  return EFI_SUCCESS;
}

/**
  Returns the address of the memory-mapped variable store FV.

  @param[in]  This     The protocol instance.
  @param[out] Address  The address; virtual once the OS has set up its
                       virtual address map.

  @retval EFI_SUCCESS            The address was returned.
  @retval EFI_INVALID_PARAMETER  Address is NULL.
**/
EFI_STATUS
EFIAPI
NvStoreFvbGetPhysicalAddress (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT       EFI_PHYSICAL_ADDRESS                 *Address
  )
{
  if (Address == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Address = (EFI_PHYSICAL_ADDRESS)(UINTN)mNvStore;
  return EFI_SUCCESS;
}

/**
  Returns the size of a block and the number of blocks from it to the end
  of the FV, all of the same size.

  @param[in]  This            The protocol instance.
  @param[in]  Lba             The block.
  @param[out] BlockSize       Its size in bytes.
  @param[out] NumberOfBlocks  The blocks from Lba on.

  @retval EFI_SUCCESS            The sizes were returned.
  @retval EFI_INVALID_PARAMETER  Lba is out of range, or an output is NULL.
**/
EFI_STATUS
EFIAPI
NvStoreFvbGetBlockSize (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN        EFI_LBA                              Lba,
  OUT       UINTN                                *BlockSize,
  OUT       UINTN                                *NumberOfBlocks
  )
{
  if ((BlockSize == NULL) || (NumberOfBlocks == NULL) || (Lba >= NVSTORE_BLOCK_COUNT)) {
    return EFI_INVALID_PARAMETER;
  }

  *BlockSize      = QCS6490_NVSTORE_BLOCK_SIZE;
  *NumberOfBlocks = NVSTORE_BLOCK_COUNT - (UINTN)Lba;
  return EFI_SUCCESS;
}

/**
  Reads from a block of the variable store, up to the end of the block.

  @param[in]      This      The protocol instance.
  @param[in]      Lba       The block.
  @param[in]      Offset    The offset in the block.
  @param[in, out] NumBytes  On input the bytes wanted, on output the bytes
                            read.
  @param[out]     Buffer    The data.

  @retval EFI_SUCCESS            The data was read.
  @retval EFI_BAD_BUFFER_SIZE    The read would have crossed the end of the
                                 block; NumBytes bytes were read.
  @retval EFI_ACCESS_DENIED      The FV is read disabled.
  @retval EFI_INVALID_PARAMETER  Lba or Offset is out of range, or a
                                 pointer is NULL.
**/
EFI_STATUS
EFIAPI
NvStoreFvbRead (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN        EFI_LBA                              Lba,
  IN        UINTN                                Offset,
  IN OUT    UINTN                                *NumBytes,
  IN OUT    UINT8                                *Buffer
  )
{
  UINTN  Count;

  if ((NumBytes == NULL) || (Buffer == NULL) ||
      (Lba >= NVSTORE_BLOCK_COUNT) || (Offset >= QCS6490_NVSTORE_BLOCK_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((mFvbAttributes & EFI_FVB2_READ_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }

  Count = MIN (*NumBytes, QCS6490_NVSTORE_BLOCK_SIZE - Offset);
  CopyMem (Buffer, mNvStore + (UINTN)Lba * QCS6490_NVSTORE_BLOCK_SIZE + Offset, Count);

  if (Count < *NumBytes) {
    *NumBytes = Count;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

/**
  Writes to a block of the variable store, up to the end of the block, and
  writes the block back to UFS when that is allowed and possible.

  @param[in]      This      The protocol instance.
  @param[in]      Lba       The block.
  @param[in]      Offset    The offset in the block.
  @param[in, out] NumBytes  On input the bytes to write, on output the bytes
                            written.
  @param[in]      Buffer    The data.

  @retval EFI_SUCCESS            The data was written.
  @retval EFI_BAD_BUFFER_SIZE    The write would have crossed the end of the
                                 block; NumBytes bytes were written.
  @retval EFI_ACCESS_DENIED      The FV is write disabled.
  @retval EFI_WRITE_PROTECTED    Boot services are over.
  @retval EFI_INVALID_PARAMETER  Lba or Offset is out of range, or a
                                 pointer is NULL.
**/
EFI_STATUS
EFIAPI
NvStoreFvbWrite (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN        EFI_LBA                              Lba,
  IN        UINTN                                Offset,
  IN OUT    UINTN                                *NumBytes,
  IN        UINT8                                *Buffer
  )
{
  UINTN    Count;
  EFI_TPL  Tpl;

  //
  // Nothing is written back at runtime: refuse, before touching anything
  // that only exists at boot time.
  //
  if (EfiAtRuntime ()) {
    return EFI_WRITE_PROTECTED;
  }

  if ((NumBytes == NULL) || (Buffer == NULL) ||
      (Lba >= NVSTORE_BLOCK_COUNT) || (Offset >= QCS6490_NVSTORE_BLOCK_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((mFvbAttributes & EFI_FVB2_WRITE_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }

  Count = MIN (*NumBytes, QCS6490_NVSTORE_BLOCK_SIZE - Offset);
  if (Count != 0) {
    Tpl = NvStoreEnter ();
    CopyMem (mNvStore + (UINTN)Lba * QCS6490_NVSTORE_BLOCK_SIZE + Offset, Buffer, Count);
    mDirty[Lba] = TRUE;
    NvStoreCommit (Tpl);
    NvStoreLeave (Tpl);
  }

  if (Count < *NumBytes) {
    *NumBytes = Count;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

/**
  Erases blocks of the variable store to 0xFF, and writes them back to UFS
  when that is allowed and possible.

  @param[in]  This  The protocol instance.
  @param[in]  ...   (EFI_LBA first block, UINTN block count) pairs, ended by
                    EFI_LBA_LIST_TERMINATOR.

  @retval EFI_SUCCESS            The blocks were erased.
  @retval EFI_ACCESS_DENIED      The FV is write disabled.
  @retval EFI_WRITE_PROTECTED    Boot services are over.
  @retval EFI_INVALID_PARAMETER  A range is empty or out of the FV; nothing
                                 was erased.
**/
EFI_STATUS
EFIAPI
NvStoreFvbEraseBlocks (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  ...
  )
{
  VA_LIST  Args;
  EFI_LBA  StartLba;
  UINTN    Count;
  UINTN    Range;
  EFI_TPL  Tpl;

  if (EfiAtRuntime ()) {
    return EFI_WRITE_PROTECTED;
  }

  if ((mFvbAttributes & EFI_FVB2_WRITE_STATUS) == 0) {
    return EFI_ACCESS_DENIED;
  }

  //
  // Check every range before erasing any.
  //
  VA_START (Args, This);
  for (Range = 0; ; Range++) {
    StartLba = VA_ARG (Args, EFI_LBA);
    if (StartLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    Count = VA_ARG (Args, UINTN);
    if ((Range >= NVSTORE_MAX_ERASE_RANGES) ||
        (Count == 0) ||
        (StartLba >= NVSTORE_BLOCK_COUNT) ||
        (Count > NVSTORE_BLOCK_COUNT - (UINTN)StartLba))
    {
      VA_END (Args);
      return EFI_INVALID_PARAMETER;
    }
  }

  VA_END (Args);

  Tpl = NvStoreEnter ();

  VA_START (Args, This);
  for (Range = 0; Range < NVSTORE_MAX_ERASE_RANGES; Range++) {
    StartLba = VA_ARG (Args, EFI_LBA);
    if (StartLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    Count = VA_ARG (Args, UINTN);
    SetMem (
      mNvStore + (UINTN)StartLba * QCS6490_NVSTORE_BLOCK_SIZE,
      Count * QCS6490_NVSTORE_BLOCK_SIZE,
      0xFF
      );
    SetMem (&mDirty[StartLba], Count, TRUE);
  }

  VA_END (Args);

  NvStoreCommit (Tpl);
  NvStoreLeave (Tpl);
  return EFI_SUCCESS;
}

/**
  Tells the OS, through the EFI_RT_PROPERTIES_TABLE, that SetVariable() is
  not available at runtime: the store reaches UFS only while boot services
  run, and the FVB refuses writes afterwards. Linux then keeps efivarfs
  read-only instead of failing every write.
**/
STATIC
VOID
NvStorePublishRtProperties (
  VOID
  )
{
  EFI_RT_PROPERTIES_TABLE  *Table;
  EFI_STATUS               Status;

  Table = AllocateRuntimePool (sizeof (*Table));
  if (Table == NULL) {
    return;
  }

  Table->Version                  = EFI_RT_PROPERTIES_TABLE_VERSION;
  Table->Length                   = sizeof (*Table);
  Table->RuntimeServicesSupported = EFI_RT_SUPPORTED_GET_TIME |
                                    EFI_RT_SUPPORTED_SET_TIME |
                                    EFI_RT_SUPPORTED_GET_WAKEUP_TIME |
                                    EFI_RT_SUPPORTED_SET_WAKEUP_TIME |
                                    EFI_RT_SUPPORTED_GET_VARIABLE |
                                    EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
                                    EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
                                    EFI_RT_SUPPORTED_CONVERT_POINTER |
                                    EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
                                    EFI_RT_SUPPORTED_RESET_SYSTEM |
                                    EFI_RT_SUPPORTED_UPDATE_CAPSULE |
                                    EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES |
                                    EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO;

  Status = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Table);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: %r\n", __func__, Status));
    FreePool (Table);
  }
}

/**
  Entry point: sets up the store in memory, produces the FVB for it, and
  lets the variable driver start.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The FVB is installed.
  @retval Other        It could not be installed.
**/
EFI_STATUS
EFIAPI
NvStoreFvbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS   Status;
  EFI_EVENT    Event;
  EFI_EVENT    BeforeExitBootServicesEvent;
  EFI_EVENT    VirtualAddressChangeEvent;
  BOOLEAN      MemoryReserved;
  CONST CHAR8  *Problem;

  BeforeExitBootServicesEvent = NULL;
  VirtualAddressChangeEvent   = NULL;

  mNvStore = (UINT8 *)(UINTN)QCS6490_NVSTORE_BASE;
  NvStoreReadPartitionName ();

  MemoryReserved = NvStoreCheckMemory ();
  NvStoreReadStatus ();

  //
  // Use what SEC loaded if it says it loaded a valid store and memory still
  // holds one. Format the store in memory otherwise.
  //
  if (!MemoryReserved) {
    DEBUG ((DEBUG_ERROR, "%a: formatting the variable store in memory, its memory was not reserved\n", __func__));
    NvStoreFormat ();
  } else if (mSecLoadResult != QCS6490_NVSTORE_LOAD_OK) {
    DEBUG ((
      DEBUG_INFO,
      "%a: formatting the variable store in memory (SEC: %a)\n",
      __func__,
      NvStoreLoadResultName (mSecLoadResult)
      ));
    NvStoreFormat ();
  } else {
    Problem = NvStoreCheckHeaders ();
    if (Problem != NULL) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: SEC loaded the variable store, but %a: formatting it\n",
        __func__,
        Problem
        ));
      NvStoreFormat ();
    } else {
      DEBUG ((
        DEBUG_INFO,
        "%a: variable store loaded by SEC, 0x%x bytes at 0x%lx\n",
        __func__,
        (UINT32)QCS6490_NVSTORE_SIZE,
        (UINT64)(UINTN)mNvStore
        ));
    }
  }

  //
  // Write back only to a partition SEC could read: memory then holds what is,
  // or is to be, on it.
  //
  if (mPartitionName[0] == L'\0') {
    mWriteBackOffReason = "PcdNvStorePartitionName is empty";
  } else if (!MemoryReserved) {
    mWriteBackOffReason = "the store's memory was not reserved";
  } else if ((mSecLoadResult != QCS6490_NVSTORE_LOAD_OK) && (mSecLoadResult != QCS6490_NVSTORE_LOAD_BLANK)) {
    mWriteBackOffReason = "SEC could not read the partition";
  } else {
    mWriteBackAllowed = TRUE;
  }

  //
  // The last write-back happens in the BeforeExitBootServices group: without
  // it, UFS could be used after the SMMU stream is gone.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NvStoreOnBeforeExitBootServices,
                  NULL,
                  &gEfiEventBeforeExitBootServicesGuid,
                  &BeforeExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: BeforeExitBootServices event: %r\n", __func__, Status));
    BeforeExitBootServicesEvent = NULL;
    mWriteBackAllowed           = FALSE;
    mWriteBackOffReason         = "no BeforeExitBootServices event";
  }

  if (mWriteBackAllowed) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    NvStoreOnFlushEvent,
                    NULL,
                    &mFlushEvent
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "%a: write-back event: %r, SetVariable () changes reach UFS only at ReadyToBoot, reset or ExitBootServices\n",
        __func__,
        Status
        ));
      mFlushEvent = NULL;
    }
  }

  if (mWriteBackAllowed) {
    DEBUG ((
      DEBUG_INFO,
      "%a: variable changes are written back to %s on UFS LUN %u\n",
      __func__,
      mPartitionName,
      FixedPcdGet8 (PcdNvStoreUfsLun)
      ));
  } else {
    DEBUG ((
      DEBUG_WARN,
      "%a: variables are kept in memory only for this boot: %a (SEC: %a)\n",
      __func__,
      mWriteBackOffReason,
      NvStoreLoadResultName (mSecLoadResult)
      ));
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  NvStoreOnVirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &VirtualAddressChangeEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: virtual address change event: %r\n", __func__, Status));
    VirtualAddressChangeEvent = NULL;
  }

  //
  // The store is valid in memory: produce the FVB, then let the variable
  // driver start (it waits for gEdkiiNvVarStoreFormattedGuid).
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mFvbHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mFvbDevicePath,
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &mFvbProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: installing the FVB: %r\n", __func__, Status));
    goto CloseEvents;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEdkiiNvVarStoreFormattedGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: installing gEdkiiNvVarStoreFormattedGuid: %r\n", __func__, Status));
    gBS->UninstallMultipleProtocolInterfaces (
           mFvbHandle,
           &gEfiDevicePathProtocolGuid,
           &mFvbDevicePath,
           &gEfiFirmwareVolumeBlockProtocolGuid,
           &mFvbProtocol,
           NULL
           );
    goto CloseEvents;
  }

  NvStorePublishRtProperties ();

  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, NvStoreOnReadyToBoot, NULL, &Event);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: ReadyToBoot event: %r\n", __func__, Status));
  }

  if (mWriteBackAllowed) {
    EfiCreateProtocolNotifyEvent (
      &gEfiResetNotificationProtocolGuid,
      TPL_CALLBACK,
      NvStoreOnResetNotificationInstalled,
      NULL,
      &mResetNotificationRegistration
      );
  }

  //
  // Watch for the partition, also while write-back is off, to log whether it
  // is there.
  //
  if (mPartitionName[0] != L'\0') {
    EfiCreateProtocolNotifyEvent (
      &gEfiBlockIoProtocolGuid,
      TPL_CALLBACK,
      NvStoreOnBlockIo,
      NULL,
      &mBlockIoRegistration
      );
    NvStoreFindPartition ();
  }

  return EFI_SUCCESS;

CloseEvents:
  //
  // The image is unloaded when its entry point fails: none of its
  // notification functions may be left registered.
  //
  if (mFlushEvent != NULL) {
    gBS->CloseEvent (mFlushEvent);
    mFlushEvent = NULL;
  }

  if (BeforeExitBootServicesEvent != NULL) {
    gBS->CloseEvent (BeforeExitBootServicesEvent);
  }

  if (VirtualAddressChangeEvent != NULL) {
    gBS->CloseEvent (VirtualAddressChangeEvent);
  }

  return Status;
}
