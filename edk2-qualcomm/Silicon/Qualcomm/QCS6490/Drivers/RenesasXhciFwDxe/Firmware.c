/** @file
  Reads the Renesas uPD720201/uPD720202 firmware from where Linux reads it:
  the file PcdRenesasXhciFwPath ("\renesas_usb_fw.mem") on the GPT partition
  named PcdRenesasXhciFwPartition ("usb_fw", an ext4 file system on UFS
  LUN 3 of the RUBIK Pi 3, mounted at /var/usbfw), through whatever file
  system driver binds to it (Ext4Dxe). Only partitions on UFS are used: a
  USB disk could carry a partition of the same name.

  The driver binding asks for the image the first time a controller needs
  it, which can be before the boot manager has connected UFS (USB consoles
  are connected first). Then only the UFS host controller is connected, never
  every controller: that would connect the xHCI being loaded again.

  The firmware is proprietary and every board carries it on that partition;
  it is never built into the firmware volume.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "RenesasXhciFw.h"

#include <Guid/FileInfo.h>
#include <Guid/NonDiscoverableDevice.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

//
// The image, once read, or why it could not be: one attempt per boot.
//
STATIC UINT8       *mImage;
STATIC UINTN       mImageSize;
STATIC UINT16      mImageVersion;
STATIC BOOLEAN     mImageTried;
STATIC EFI_STATUS  mImageStatus;
STATIC BOOLEAN     mImageReading;

/**
  Reads a whole file.

  @param[in]   Root  The volume.
  @param[in]   Path  The file.
  @param[out]  Data  Its contents, from the pool.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  No such file, or not one that can be the firmware.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT UINT8              **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              ReadSize;
  UINT8              *Buffer;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %s: %r\n", __func__, Path, Status));
    return EFI_NOT_FOUND;
  }

  InfoSize = 0;
  Info     = NULL;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %s: no file info: %r\n", __func__, Path, Status));
    goto Close;
  }

  //
  // renesas_fw_verify() rejects anything of 64 KiB or more; do not read it.
  //
  if (((Info->Attribute & EFI_FILE_DIRECTORY) != 0) || (Info->FileSize == 0) ||
      (Info->FileSize >= RENESAS_FW_MAX_SIZE))
  {
    DEBUG ((DEBUG_ERROR, "%a: %s: not a firmware file (%lu bytes)\n", __func__, Path, Info->FileSize));
    Status = EFI_NOT_FOUND;
    goto Close;
  }

  ReadSize = (UINTN)Info->FileSize;
  Buffer   = AllocatePool (ReadSize);
  if (Buffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Close;
  }

  Status = File->Read (File, &ReadSize, Buffer);
  if (EFI_ERROR (Status) || (ReadSize != Info->FileSize)) {
    DEBUG ((DEBUG_ERROR, "%a: %s: read %lu of %lu bytes: %r\n", __func__, Path, (UINT64)ReadSize, Info->FileSize, Status));
    FreePool (Buffer);
    Status = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    goto Close;
  }

  *Data = Buffer;
  *Size = ReadSize;

Close:
  if (Info != NULL) {
    FreePool (Info);
  }

  File->Close (File);
  return Status;
}

/**
  Reads the firmware file from one volume.

  @param[in]   Handle  The volume's handle.
  @param[in]   Path    The file.
  @param[out]  Data    The file.
  @param[out]  Size    Its size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  No file system, or no such file on it.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareReadFromVolume (
  IN  EFI_HANDLE    Handle,
  IN  CONST CHAR16  *Path,
  OUT UINT8         **Data,
  OUT UINTN         *Size
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no file system on the partition (is Ext4Dxe built in?)\n", __func__));
    return EFI_NOT_FOUND;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: OpenVolume: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  Status = FirmwareReadFile (Root, Path, Data, Size);
  Root->Close (Root);
  return Status;
}

/**
  Returns whether a handle is on UFS: its device path goes through a UFS
  device.

  @param[in]  Handle  The handle.

  @return  TRUE if it is.
**/
STATIC
BOOLEAN
FirmwareIsOnUfs (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  Node = DevicePathFromHandle (Handle);
  if (Node == NULL) {
    return FALSE;
  }

  for ( ; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MSG_UFS_DP))
    {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Looks for the firmware on the UFS GPT partitions named
  PcdRenesasXhciFwPartition that are already there, binding their file
  system first.

  @param[out]  Data     The file.
  @param[out]  Size     Its size.
  @param[out]  Volumes  How many such partitions were looked at.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  Not found.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareSearch (
  OUT UINT8  **Data,
  OUT UINTN  *Size,
  OUT UINTN  *Volumes
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  CONST CHAR16                 *PartitionName;
  CONST CHAR16                 *Path;

  PartitionName = (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPartition);
  Path          = (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPath);
  *Volumes      = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount && Status == EFI_NOT_FOUND; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartitionInfo)) ||
        (PartitionInfo->Type != PARTITION_TYPE_GPT) ||
        (StrnCmp (PartitionInfo->Info.Gpt.PartitionName, PartitionName, ARRAY_SIZE (PartitionInfo->Info.Gpt.PartitionName)) != 0))
    {
      continue;
    }

    //
    // A USB disk could carry a partition of the same name; only the board's
    // own storage is trusted to hold the firmware.
    //
    if (!FirmwareIsOnUfs (Handles[Index]) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)))
    {
      continue;
    }

    //
    // Binds Ext4Dxe if nothing has connected the partition yet.
    //
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);

    (*Volumes)++;
    Status = FirmwareReadFromVolume (Handles[Index], Path, Data, Size);
    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "%a: %s%s, %lu bytes (block size %u)\n",
        __func__,
        PartitionName,
        Path,
        (UINT64)*Size,
        BlockIo->Media->BlockSize
        ));
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Connects the UFS host controllers, and everything below them, so that
  their partitions appear: the NonDiscoverableDevice handles of type UFS
  (QcomUfsHcDxe registers one). Nothing else is connected.

  @return  How many UFS host controllers were connected.
**/
STATIC
UINTN
FirmwareConnectUfs (
  VOID
  )
{
  EFI_STATUS               Status;
  EFI_HANDLE               *Handles;
  UINTN                    HandleCount;
  UINTN                    Index;
  UINTN                    Connected;
  NON_DISCOVERABLE_DEVICE  *Device;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEdkiiNonDiscoverableDeviceProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no non-discoverable devices: %r\n", __func__, Status));
    return 0;
  }

  Connected = 0;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEdkiiNonDiscoverableDeviceProtocolGuid, (VOID **)&Device)) ||
        (Device->Type == NULL) ||
        !CompareGuid (Device->Type, &gEdkiiNonDiscoverableUfsDeviceGuid))
    {
      continue;
    }

    Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    DEBUG ((DEBUG_INFO, "%a: UFS host %p: %r\n", __func__, Handles[Index], Status));
    Connected++;
  }

  FreePool (Handles);
  return Connected;
}

/**
  Checks an image as renesas_fw_verify() does (xhci-pci-renesas.c:122-156),
  and also that it is whole dwords: the download sends Size / 4 dwords and
  would silently drop a tail.

  @param[in]   Data     The image.
  @param[in]   Size     Its size.
  @param[out]  Version  The version it carries, at its version pointer.

  @retval EFI_SUCCESS            It can be downloaded.
  @retval EFI_COMPROMISED_DATA   It cannot.
**/
STATIC
EFI_STATUS
FirmwareValidate (
  IN  CONST UINT8  *Data,
  IN  UINTN        Size,
  OUT UINT16       *Version
  )
{
  UINT16  Signature;
  UINT16  Pointer;

  if ((Size < RENESAS_FW_MIN_SIZE) || (Size >= RENESAS_FW_MAX_SIZE)) {
    DEBUG ((DEBUG_ERROR, "%a: %lu bytes, not 4 KiB to 64 KiB\n", __func__, (UINT64)Size));
    return EFI_COMPROMISED_DATA;
  }

  if ((Size % sizeof (UINT32)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: %lu bytes, not whole dwords\n", __func__, (UINT64)Size));
    return EFI_COMPROMISED_DATA;
  }

  //
  // "LSB on left": the first two bytes are AA 55.
  //
  Signature = ReadUnaligned16 ((CONST UINT16 *)Data);
  if (Signature != RENESAS_FW_SIGNATURE) {
    DEBUG ((DEBUG_ERROR, "%a: header 0x%04x, not 0x%04x\n", __func__, Signature, RENESAS_FW_SIGNATURE));
    return EFI_COMPROMISED_DATA;
  }

  Pointer = ReadUnaligned16 ((CONST UINT16 *)(Data + RENESAS_FW_VERSION_POINTER_OFFSET));
  if ((UINTN)Pointer + 2 >= Size) {
    DEBUG ((DEBUG_ERROR, "%a: version pointer 0x%x outside the image\n", __func__, Pointer));
    return EFI_COMPROMISED_DATA;
  }

  *Version = ReadUnaligned16 ((CONST UINT16 *)(Data + Pointer));
  DEBUG ((
    DEBUG_INFO,
    "%a: %lu bytes (%lu dwords), version pointer 0x%x, version %04x\n",
    __func__,
    (UINT64)Size,
    (UINT64)(Size / sizeof (UINT32)),
    Pointer,
    *Version
    ));
  return EFI_SUCCESS;
}

/**
  Reads the image: from a partition already there, or else after connecting
  the UFS host controllers.

  @param[out]  Data  The image, from the pool.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS  Read.
  @retval Other        Not found, or it could not be read.
**/
STATIC
EFI_STATUS
FirmwareLoad (
  OUT UINT8  **Data,
  OUT UINTN  *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Volumes;
  UINTN       UfsHosts;

  Status = FirmwareSearch (Data, Size, &Volumes);
  if ((Status == EFI_NOT_FOUND) && (Volumes == 0)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: no %s partition yet, connecting the UFS host\n",
      __func__,
      (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPartition)
      ));
    UfsHosts = FirmwareConnectUfs ();
    if (UfsHosts == 0) {
      DEBUG ((DEBUG_ERROR, "%a: no UFS host controller to connect\n", __func__));
    }

    Status = FirmwareSearch (Data, Size, &Volumes);
  }

  if ((Status == EFI_NOT_FOUND) && (Volumes == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no GPT partition named %s on UFS\n",
      __func__,
      (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPartition)
      ));
  }

  return Status;
}

/**
  Returns the firmware image, read from the file PcdRenesasXhciFwPath on the
  GPT partition named PcdRenesasXhciFwPartition and checked as Linux checks
  it.

  The first call reads it, connecting the UFS host first if the partition is
  not there yet. The image is kept for the rest of the boot, and so is a
  failure: it is reported once, and later calls return the same error at
  once.

  @param[out]  Image    The image. Owned by this module; never freed.
  @param[out]  Size     Its size in bytes, a multiple of 4.
  @param[out]  Version  The firmware version the image carries.

  @retval EFI_SUCCESS    The image is there.
  @retval EFI_NOT_READY  Called while the image is being read.
  @retval Other          There is no usable image this boot.
**/
EFI_STATUS
RenesasFirmwareGet (
  OUT CONST UINT8  **Image,
  OUT UINTN        *Size,
  OUT UINT16       *Version
  )
{
  EFI_STATUS  Status;
  UINT8       *Data;
  UINTN       DataSize;
  UINT16      DataVersion;
  UINT64      Start;

  if (mImage != NULL) {
    *Image   = mImage;
    *Size    = mImageSize;
    *Version = mImageVersion;
    return EFI_SUCCESS;
  }

  if (mImageTried) {
    return mImageStatus;
  }

  //
  // Connecting UFS runs every driver binding's Supported() on the new
  // handles, this module's included; it must not start a second read.
  //
  if (mImageReading) {
    return EFI_NOT_READY;
  }

  mImageReading = TRUE;
  Start         = GetPerformanceCounter ();

  Data     = NULL;
  DataSize = 0;
  Status   = FirmwareLoad (&Data, &DataSize);
  if (!EFI_ERROR (Status)) {
    Status = FirmwareValidate (Data, DataSize, &DataVersion);
    if (EFI_ERROR (Status)) {
      FreePool (Data);
    }
  }

  mImageReading = FALSE;
  mImageTried   = TRUE;
  mImageStatus  = Status;

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no usable firmware %s on %s (%r); not trying again this boot\n",
      __func__,
      (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPath),
      (CONST CHAR16 *)PcdGetPtr (PcdRenesasXhciFwPartition),
      Status
      ));
    return Status;
  }

  mImage        = Data;
  mImageSize    = DataSize;
  mImageVersion = DataVersion;
  DEBUG ((
    DEBUG_INFO,
    "%a: firmware %04x ready after %lu ms\n",
    __func__,
    mImageVersion,
    DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - Start), 1000000)
    ));

  *Image   = mImage;
  *Size    = mImageSize;
  *Version = mImageVersion;
  return EFI_SUCCESS;
}
