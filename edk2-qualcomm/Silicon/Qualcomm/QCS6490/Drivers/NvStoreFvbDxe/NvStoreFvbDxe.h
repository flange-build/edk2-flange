/** @file
  Firmware Volume Block driver for the QCS6490 UEFI variable store, which SEC
  reads from UFS into memory and this driver writes back.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef NV_STORE_FVB_DXE_H_
#define NV_STORE_FVB_DXE_H_

#include <PiDxe.h>
#include <Library/PcdLib.h>

#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/PartitionInfo.h>

#include <Qcs6490NvStore.h>

//
// Blocks of the store: variable + FTW working + FTW spare, 4 KiB each.
//
#define NVSTORE_BLOCK_COUNT  (QCS6490_NVSTORE_SIZE / QCS6490_NVSTORE_BLOCK_SIZE)

//
// FV header followed by one block map entry and the terminating entry.
//
#define NVSTORE_FV_HEADER_LENGTH  (sizeof (EFI_FIRMWARE_VOLUME_HEADER) + sizeof (EFI_FV_BLOCK_MAP_ENTRY))

//
// Size of the variable store header's region: the variable area less the FV
// header.
//
#define NVSTORE_VARIABLE_STORE_SIZE  (FixedPcdGet32 (PcdFlashNvStorageVariableSize) - NVSTORE_FV_HEADER_LENGTH)

//
// FV attributes the store is formatted with, as RkFvbDxe does.
//
#define NVSTORE_FVB_ATTRIBUTES  (EFI_FVB2_READ_ENABLED_CAP  |  \
                                 EFI_FVB2_READ_STATUS       |  \
                                 EFI_FVB2_STICKY_WRITE      |  \
                                 EFI_FVB2_ERASE_POLARITY    |  \
                                 EFI_FVB2_WRITE_STATUS      |  \
                                 EFI_FVB2_WRITE_ENABLED_CAP |  \
                                 EFI_FVB2_MEMORY_MAPPED)

//
// FVB attributes: capability and status bits.
//
#define NVSTORE_FVB2_CAPABILITIES  (EFI_FVB2_READ_DISABLED_CAP  |  \
                                    EFI_FVB2_READ_ENABLED_CAP   |  \
                                    EFI_FVB2_WRITE_DISABLED_CAP |  \
                                    EFI_FVB2_WRITE_ENABLED_CAP  |  \
                                    EFI_FVB2_LOCK_CAP)

//
// The only FVB attributes SetAttributes () changes; every other bit must stay
// as it is.
//
#define NVSTORE_FVB2_STATUS  (EFI_FVB2_READ_STATUS  |  \
                              EFI_FVB2_WRITE_STATUS |  \
                              EFI_FVB2_LOCK_STATUS)

//
// Characters in a GPT partition name (EFI_PARTITION_ENTRY.PartitionName).
//
#define NVSTORE_GPT_NAME_LENGTH  36

//
// Bounds for the loops over caller-supplied data: device path nodes looked
// at for the UFS node, and (LBA, count) pairs of one EraseBlocks () call.
//
#define NVSTORE_MAX_DEVICE_PATH_NODES  64
#define NVSTORE_MAX_ERASE_RANGES       1024

//
// Write-back attempts that may fail in a row before the driver stops
// writing to UFS for the rest of the boot, so that a broken device does not
// cost a UFS timeout on every variable write.
//
#define NVSTORE_MAX_WRITE_FAILURES  3

//
// Vendor hardware device path node of the FVB handle.
//
#define NVSTORE_FVB_DEVICE_PATH_GUID \
  { 0x6c41bb8f, 0x1dbc, 0x4bf1, { 0x8a, 0x9d, 0xa1, 0xe3, 0x55, 0x07, 0x91, 0xff } }

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} NVSTORE_FVB_DEVICE_PATH;

//
// The start of the store as formatted: the FV header with its one block map
// entry (BlockMap[0]), the terminating block map entry, then the variable
// store header, where the variables begin.
//
typedef struct {
  EFI_FIRMWARE_VOLUME_HEADER    FvHeader;
  EFI_FV_BLOCK_MAP_ENTRY        BlockMapEnd;
  VARIABLE_STORE_HEADER         VariableStore;
} NVSTORE_HEADERS;

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
  );

/**
  Changes the status bits of the variable store FV attributes for this boot.

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
  );

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
  );

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
  );

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
  );

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
  );

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
  );

#endif // NV_STORE_FVB_DXE_H_
