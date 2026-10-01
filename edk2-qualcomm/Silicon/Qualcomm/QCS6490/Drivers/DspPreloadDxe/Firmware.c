/** @file
  Reads DSP firmware from the OS's root file system, where Linux's firmware
  loader finds it: usr/lib/firmware/updates first, then usr/lib/firmware
  (drivers/base/firmware_loader/main.c fw_path). The uncompressed files are
  taken; Linux tries those before any .zst.

  The root file system is the partition named PcdDspFirmwarePartition, read
  through whatever file system driver binds to it (Ext4Dxe for flange's ext4
  root). Partitions on removable media are tried after the others.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/FileInfo.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include "DspPreload.h"

//
// Larger files are not DSP images; the largest carve-out is 40 MiB.
//
#define FIRMWARE_MAX_SIZE  SIZE_64MB

#define FIRMWARE_PATH_LENGTH  256

STATIC CONST CHAR16  *mFirmwareDirs[] = {
  L"\\usr\\lib\\firmware\\updates\\",
  L"\\usr\\lib\\firmware\\",
};

/**
  Reads a whole file.

  @param[in]   Root  The volume.
  @param[in]   Path  The file.
  @param[out]  Data  Its contents, from the pool.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  No such file.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CHAR16             *Path,
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

  Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
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
    goto Close;
  }

  if (((Info->Attribute & EFI_FILE_DIRECTORY) != 0) || (Info->FileSize == 0) ||
      (Info->FileSize > FIRMWARE_MAX_SIZE))
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
  Looks for a firmware file on one volume.

  @param[in]   Handle  The volume's handle.
  @param[in]   Name    The firmware name, '/' separated.
  @param[out]  Data    The file.
  @param[out]  Size    Its size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  Not on this volume.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareReadFromVolume (
  IN  EFI_HANDLE   Handle,
  IN  CONST CHAR8  *Name,
  OUT UINT8        **Data,
  OUT UINTN        *Size
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  CHAR16                           Path[FIRMWARE_PATH_LENGTH];
  UINTN                            Dir;
  UINTN                            Length;
  UINTN                            Index;

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Dir = 0; Dir < ARRAY_SIZE (mFirmwareDirs) && Status == EFI_NOT_FOUND; Dir++) {
    StrCpyS (Path, ARRAY_SIZE (Path), mFirmwareDirs[Dir]);
    Length = StrLen (Path);
    for (Index = 0; Name[Index] != '\0' && Length + 1 < ARRAY_SIZE (Path); Index++) {
      Path[Length++] = (Name[Index] == '/') ? L'\\' : (CHAR16)Name[Index];
    }

    Path[Length] = L'\0';
    Status       = FirmwareReadFile (Root, Path, Data, Size);
    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "%a: %s, %lu bytes\n", __func__, Path, (UINT64)*Size));
    }
  }

  Root->Close (Root);
  return Status;
}

/**
  Connects every controller, as the boot manager's Connect All does, for a
  boot whose discovery policy left the disks unconnected.
**/
STATIC
VOID
FirmwareConnectAll (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Previous;
  UINTN       Index;

  Previous = 0;
  for ( ; ;) {
    Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &HandleCount, &Handles);
    if (EFI_ERROR (Status)) {
      return;
    }

    for (Index = 0; Index < HandleCount; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }

    FreePool (Handles);

    //
    // New handles (a host controller's devices, their partitions) may
    // need connecting in turn.
    //
    if (HandleCount == Previous) {
      return;
    }

    Previous = HandleCount;
  }
}

/**
  Looks for a firmware file on the volumes named PcdDspFirmwarePartition
  that are already there.

  @param[in]   Name     The firmware name.
  @param[out]  Data     The file.
  @param[out]  Size     Its size.
  @param[out]  Volumes  How many volumes were looked at.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  Not found.
  @retval Other          It could not be read.
**/
STATIC
EFI_STATUS
FirmwareSearch (
  IN  CONST CHAR8  *Name,
  OUT UINT8        **Data,
  OUT UINTN        *Size,
  OUT UINTN        *Volumes
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        Index;
  UINTN                        Pass;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  CONST CHAR16                 *PartitionName;

  PartitionName = (CONST CHAR16 *)PcdGetPtr (PcdDspFirmwarePartition);
  *Volumes      = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  //
  // Fixed media first, then removable media.
  //
  for (Pass = 0; Pass < 2 && Status == EFI_NOT_FOUND; Pass++) {
    for (Index = 0; Index < HandleCount && Status == EFI_NOT_FOUND; Index++) {
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartitionInfo)) ||
          (PartitionInfo->Type != PARTITION_TYPE_GPT) ||
          (StrnCmp (PartitionInfo->Info.Gpt.PartitionName, PartitionName, ARRAY_SIZE (PartitionInfo->Info.Gpt.PartitionName)) != 0))
      {
        continue;
      }

      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
          (BlockIo->Media->RemovableMedia != (Pass == 1)))
      {
        continue;
      }

      //
      // Boot discovery normally has the file system bound already.
      //
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);

      (*Volumes)++;
      Status = FirmwareReadFromVolume (Handles[Index], Name, Data, Size);
    }
  }

  FreePool (Handles);
  return Status;
}

EFI_STATUS
FirmwareRead (
  IN  CONST CHAR8  *Name,
  OUT UINT8        **Data,
  OUT UINTN        *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Volumes;

  Status = FirmwareSearch (Name, Data, Size, &Volumes);
  if ((Status == EFI_NOT_FOUND) && (Volumes == 0)) {
    DEBUG ((DEBUG_INFO, "%a: no %s volume yet, connecting everything\n", __func__, (CHAR16 *)PcdGetPtr (PcdDspFirmwarePartition)));
    FirmwareConnectAll ();
    Status = FirmwareSearch (Name, Data, Size, &Volumes);
  }

  if (Status == EFI_NOT_FOUND) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a not found on %u volume(s) named %s\n",
      __func__,
      Name,
      (UINT32)Volumes,
      (CHAR16 *)PcdGetPtr (PcdDspFirmwarePartition)
      ));
  }

  return Status;
}
