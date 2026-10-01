/** @file
  Describes the DRAM of QCS6490 boards in SMBIOS (types 16, 17 and 19).

  The front page of the setup UI and tools like dmidecode take the memory
  size from these records. The DRAM ranges are the system memory resources
  the platform library published from the RAM partition table in SMEM,
  carve-outs included.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <IndustryStandard/SmBios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Smbios.h>

#define MAX_DRAM_RANGES  16

typedef struct {
  UINT64    Base;
  UINT64    End;
} DRAM_RANGE;

/**
  Collects the DRAM ranges from the resource HOBs, merging adjacent ones.

  @param[out]  Ranges      The ranges, sorted by address.
  @param[out]  RangeCount  The number of ranges.

  @return  The total size of the ranges.
**/
STATIC
UINT64
GetDramRanges (
  OUT DRAM_RANGE  *Ranges,
  OUT UINTN       *RangeCount
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  DRAM_RANGE            Range;
  UINTN                 Count;
  UINTN                 Index;
  UINTN                 Merged;
  UINT64                Total;

  Count = 0;
  for (Hob.Raw = GetHobList ();
       (Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw)) != NULL;
       Hob.Raw = GET_NEXT_HOB (Hob))
  {
    if ((Hob.ResourceDescriptor->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) ||
        (Count == MAX_DRAM_RANGES))
    {
      continue;
    }

    Range.Base = Hob.ResourceDescriptor->PhysicalStart;
    Range.End  = Range.Base + Hob.ResourceDescriptor->ResourceLength;

    //
    // Insert sorted by address.
    //
    for (Index = Count; Index > 0 && Ranges[Index - 1].Base > Range.Base; Index--) {
      Ranges[Index] = Ranges[Index - 1];
    }

    Ranges[Index] = Range;
    Count++;
  }

  Merged = 0;
  Total  = 0;
  for (Index = 0; Index < Count; Index++) {
    if ((Merged > 0) && (Ranges[Merged - 1].End == Ranges[Index].Base)) {
      Ranges[Merged - 1].End = Ranges[Index].End;
    } else {
      Ranges[Merged++] = Ranges[Index];
    }

    Total += Ranges[Index].End - Ranges[Index].Base;
  }

  *RangeCount = Merged;
  return Total;
}

/**
  Adds an SMBIOS record.

  @param[in]   Smbios   The SMBIOS protocol.
  @param[in]   Record   The formatted part of the record.
  @param[in]   Size     The size of Record.
  @param[in]   String   The only string of the record, or NULL.
  @param[out]  Handle   The handle of the record.

  @retval EFI_SUCCESS  The record was added.
  @retval Other        It was not.
**/
STATIC
EFI_STATUS
AddRecord (
  IN  EFI_SMBIOS_PROTOCOL  *Smbios,
  IN  VOID                 *Record,
  IN  UINTN                Size,
  IN  CONST CHAR8          *String  OPTIONAL,
  OUT EFI_SMBIOS_HANDLE    *Handle
  )
{
  EFI_STATUS  Status;
  UINT8       *Buffer;
  UINTN       StringSize;

  //
  // The string set ends with a double NUL, even when it is empty.
  //
  StringSize = (String != NULL) ? AsciiStrSize (String) + 1 : 2;
  Buffer     = AllocateZeroPool (Size + StringSize);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Buffer, Record, Size);
  if (String != NULL) {
    CopyMem (Buffer + Size, String, StringSize - 1);
  }

  *Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status  = Smbios->Add (Smbios, NULL, Handle, (EFI_SMBIOS_TABLE_HEADER *)Buffer);
  FreePool (Buffer);

  return Status;
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The records were added.
  @retval Other        They were not.
**/
EFI_STATUS
EFIAPI
SmbiosMemoryDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_SMBIOS_PROTOCOL  *Smbios;
  DRAM_RANGE           Ranges[MAX_DRAM_RANGES];
  UINTN                RangeCount;
  UINTN                Index;
  UINT64               Total;
  UINT64               Installed;
  SMBIOS_TABLE_TYPE16  Type16;
  SMBIOS_TABLE_TYPE17  Type17;
  SMBIOS_TABLE_TYPE19  Type19;
  EFI_SMBIOS_HANDLE    ArrayHandle;
  EFI_SMBIOS_HANDLE    Handle;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Total = GetDramRanges (Ranges, &RangeCount);
  if (RangeCount == 0) {
    return EFI_NOT_FOUND;
  }

  //
  // What the partition table leaves out (the TrustZone holes, and whatever
  // XBL keeps for itself) is still installed: round up to the whole GiB.
  //
  Installed = ALIGN_VALUE (Total, SIZE_1GB);

  ZeroMem (&Type16, sizeof (Type16));
  Type16.Hdr.Type                     = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  Type16.Hdr.Length                   = sizeof (Type16);
  Type16.Location                     = MemoryArrayLocationSystemBoard;
  Type16.Use                          = MemoryArrayUseSystemMemory;
  Type16.MemoryErrorCorrection        = MemoryErrorCorrectionNone;
  Type16.MaximumCapacity              = (UINT32)(Installed / SIZE_1KB);
  Type16.MemoryErrorInformationHandle = 0xFFFE;
  Type16.NumberOfMemoryDevices        = 1;
  Status                              = AddRecord (Smbios, &Type16, sizeof (Type16), NULL, &ArrayHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type17, sizeof (Type17));
  Type17.Hdr.Type                           = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  Type17.Hdr.Length                         = sizeof (Type17);
  Type17.MemoryArrayHandle                  = ArrayHandle;
  Type17.MemoryErrorInformationHandle       = 0xFFFE;
  Type17.TotalWidth                         = 0xFFFF;
  Type17.DataWidth                          = 0xFFFF;
  Type17.Size                               = (UINT16)(Installed / SIZE_1MB);
  Type17.FormFactor                         = MemoryFormFactorRowOfChips;
  Type17.DeviceLocator                      = 1;
  Type17.MemoryType                         = MemoryTypeLpddr4;
  Type17.TypeDetail.Synchronous             = 1;
  Type17.TypeDetail.Unbuffered              = 1;
  Type17.MemoryTechnology                   = MemoryTechnologyDram;
  Type17.MemoryOperatingModeCapability.Bits.VolatileMemory = 1;
  Type17.VolatileSize                       = Installed;
  Type17.LogicalSize                        = Installed;
  Status                                    = AddRecord (Smbios, &Type17, sizeof (Type17), "DRAM", &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < RangeCount; Index++) {
    ZeroMem (&Type19, sizeof (Type19));
    Type19.Hdr.Type          = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    Type19.Hdr.Length        = sizeof (Type19);
    Type19.StartingAddress   = (UINT32)(Ranges[Index].Base / SIZE_1KB);
    Type19.EndingAddress     = (UINT32)((Ranges[Index].End - 1) / SIZE_1KB);
    Type19.MemoryArrayHandle = ArrayHandle;
    Type19.PartitionWidth    = 1;
    Status                   = AddRecord (Smbios, &Type19, sizeof (Type19), NULL, &Handle);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %lu MiB in %u ranges, %lu MiB installed\n",
    __func__,
    Total / SIZE_1MB,
    RangeCount,
    Installed / SIZE_1MB
    ));

  return EFI_SUCCESS;
}
