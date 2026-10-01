/** @file
  QCS6490 memory map.

  DRAM comes from the RAM partition table XBL leaves in SMEM. The regions that
  belong to the hypervisor, TrustZone, the boot firmware and the remote
  processors are cut out of it and handed to the OS as reserved memory, so
  that neither UEFI nor the OS loader ever allocates from them; Linux booted
  through the EFI stub takes its RAM from the UEFI memory map alone. They are
  also left out of the UEFI page tables to keep speculative accesses away.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/QualcommSmemLib.h>

#include <RamPartition.h>

#include "Qcs6490LibInternal.h"

#define QCS6490_PERIPHERAL_BASE  0x00000000ULL

#define QCS6490_MAX_DRAM_BANKS  8

typedef struct {
  UINT64         Base;
  UINT64         Size;
  CONST CHAR8    *Name;
} QCS6490_MEMORY_RANGE;

//
// Carve-outs owned by the boot firmware, hypervisor, TrustZone and the remote
// processors. Sorted by address and non-overlapping.
//
// This is the union of the memory map in the uefiplat.cfg of the stock
// Qualcomm UEFI (BOOT.MXF.1.0.c1) and the no-map reserved-memory nodes of
// kodiak.dtsi and the QCS6490 board device trees, mainline and vendor. The
// stock firmware reserves more than some mainline device trees do (the whole
// remote processor area, for instance); that is what the OS has been tested
// with, so it is kept here as well. The stock UEFI also reserves its splash
// screen at 0xE1000000; without a display driver there is nothing to keep.
//
STATIC CONST QCS6490_MEMORY_RANGE  mCarveouts[] = {
  //
  // hyp, Axon DMA, xbl, aop, cmd-db, xbl log and devicetree, sec_apps, smem,
  // cpucp, wlan firmware and the CDSP secure heap.
  //
  { 0x80000000, 0x03600000, "Firmware"                 },
  //
  // camera, wpss, adsp, cdsp, spss, video, cvp, ipa, gpu and mpss.
  //
  { 0x84300000, 0x16B00000, "Remote processors"        },
  { 0x9CB80000, 0x00800000, "ADSP RPC remote heap"     },
  //
  // tz_stat, tags, qtee and trusted applications.
  //
  { 0xC0000000, 0x03400000, "TrustZone"                },
  { 0xD0600000, 0x00100000, "Debug VM"                 },
  { 0xE0000000, 0x00F00000, "DBI dump"                 },
};

/**
  Reads the DRAM banks from the RAM partition table in SMEM.

  @param[out]  Banks      The banks found, sorted by address.
  @param[out]  BankCount  The number of banks found.

  @retval EFI_SUCCESS  At least one bank was found.
  @retval Other        The table could not be read.
**/
STATIC
EFI_STATUS
GetDramBanksFromSmem (
  OUT QCS6490_MEMORY_RANGE  *Banks,
  OUT UINTN                 *BankCount
  )
{
  EFI_STATUS                  Status;
  USABLE_RAM_PARTITION_TABLE  *Table;
  RAM_PARTITION_ENTRY         *Entry;
  RAM_PARTITION_ENTRY_V1      *EntryV1;
  UINTN                       TableSize;
  UINTN                       EntrySize;
  UINTN                       Index;
  UINTN                       Sorted;
  UINT32                      Type;
  UINT32                      Category;
  UINT64                      Base;
  UINT64                      Size;
  QCS6490_MEMORY_RANGE        Bank;

  *BankCount = 0;

  Status = QualcommSmemInit ();
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
    DEBUG ((DEBUG_ERROR, "%a: SMEM init failed: %r\n", __func__, Status));
    return Status;
  }

  TableSize = 0;
  Status    = QualcommSmemLookup (
                QUALCOMM_SMEM_HOST_COMMON,
                SMEM_USABLE_RAM_PARTITION_TABLE,
                QUALCOMM_SMEM_FLAG_NONE,
                (VOID **)&Table,
                &TableSize
                );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no RAM partition table in SMEM: %r\n", __func__, Status));
    return Status;
  }

  if ((TableSize < OFFSET_OF (USABLE_RAM_PARTITION_TABLE, RamPartEntry)) ||
      (Table->Magic1 != RAM_PART_MAGIC1) ||
      (Table->Magic2 != RAM_PART_MAGIC2) ||
      (Table->Version < 1))
  {
    DEBUG ((DEBUG_ERROR, "%a: unsupported RAM partition table\n", __func__));
    return EFI_UNSUPPORTED;
  }

  EntrySize = (Table->Version == 1) ? sizeof (RAM_PARTITION_ENTRY_V1)
                                    : sizeof (RAM_PARTITION_ENTRY);
  if ((Table->NumPartitions > RAM_NUM_PART_ENTRIES) ||
      (TableSize < OFFSET_OF (USABLE_RAM_PARTITION_TABLE, RamPartEntry) +
                   Table->NumPartitions * EntrySize))
  {
    DEBUG ((DEBUG_ERROR, "%a: truncated RAM partition table\n", __func__));
    return EFI_UNSUPPORTED;
  }

  DEBUG ((
    DEBUG_INFO,
    "RAM partition table v%u, %u entries\n",
    Table->Version,
    Table->NumPartitions
    ));

  for (Index = 0; Index < Table->NumPartitions; Index++) {
    if (Table->Version == 1) {
      EntryV1  = &((USABLE_RAM_PARTITION_TABLE_V1 *)Table)->RamPartEntryV1[Index];
      Type     = EntryV1->PartitionType;
      Category = EntryV1->PartitionCategory;
      Base     = EntryV1->StartAddress;
      Size     = EntryV1->Length;
    } else {
      Entry    = &Table->RamPartEntry[Index];
      Type     = Entry->PartitionType;
      Category = Entry->PartitionCategory;
      Base     = Entry->StartAddress;
      Size     = (Entry->AvailableLength != 0) ? Entry->AvailableLength
                                               : Entry->Length;
    }

    DEBUG ((
      DEBUG_INFO,
      "  [%u] 0x%010lx-0x%010lx type %u category %u\n",
      Index,
      Base,
      Base + Size,
      Type,
      Category
      ));

    if ((Type != RamPartitionSysMemory) || (Category != RamPartitionSdram) ||
        (Size == 0))
    {
      continue;
    }

    if (((Base | Size) & EFI_PAGE_MASK) != 0) {
      DEBUG ((DEBUG_WARN, "%a: ignoring unaligned partition %u\n", __func__, Index));
      continue;
    }

    if (*BankCount == QCS6490_MAX_DRAM_BANKS) {
      DEBUG ((DEBUG_WARN, "%a: ignoring partition %u\n", __func__, Index));
      continue;
    }

    //
    // Insert sorted by address.
    //
    Bank.Base = Base;
    Bank.Size = Size;
    Bank.Name = "DRAM";
    for (Sorted = *BankCount; Sorted > 0 && Banks[Sorted - 1].Base > Base; Sorted--) {
      Banks[Sorted] = Banks[Sorted - 1];
    }

    Banks[Sorted] = Bank;
    (*BankCount)++;
  }

  return (*BankCount > 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

/**
  Describes a range of DRAM to DXE.

  @param[in]  Base        The start of the range.
  @param[in]  Size        The size of the range.
  @param[in]  Name        What the range is, for the log.
  @param[in]  Reserved    TRUE if the range is a carve-out.
  @param[out] Table       The page table descriptors.
  @param[out] TableCount  The number of descriptors used in Table.
**/
STATIC
VOID
AddDramRange (
  IN     UINT64                        Base,
  IN     UINT64                        Size,
  IN     CONST CHAR8                   *Name,
  IN     BOOLEAN                       Reserved,
  IN OUT ARM_MEMORY_REGION_DESCRIPTOR  *Table,
  IN OUT UINTN                         *TableCount
  )
{
  DEBUG ((
    DEBUG_INFO,
    "  0x%010lx-0x%010lx %a%a\n",
    Base,
    Base + Size,
    Reserved ? "reserved: " : "",
    Name
    ));

  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    EFI_RESOURCE_ATTRIBUTE_PRESENT |
    EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
    EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
    EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
    EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
    EFI_RESOURCE_ATTRIBUTE_TESTED,
    Base,
    Size
    );

  if (Reserved) {
    BuildMemoryAllocationHob (Base, Size, EfiReservedMemoryType);
    return;
  }

  Table[*TableCount].PhysicalBase = Base;
  Table[*TableCount].VirtualBase  = Base;
  Table[*TableCount].Length       = Size;
  Table[*TableCount].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  (*TableCount)++;
}

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU
  on your platform. This implementation also publishes the DRAM and carve-out
  resource HOBs, which MemoryInitPeiLib leaves to the platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR
                                    describing a Physical-to-Virtual Memory
                                    mapping. This array must be ended by a
                                    zero-filled entry. The allocated memory
                                    will not be freed.

**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *Table;
  QCS6490_MEMORY_RANGE          Banks[QCS6490_MAX_DRAM_BANKS];
  UINTN                         BankCount;
  UINTN                         TableCount;
  UINTN                         BankIndex;
  UINTN                         Index;
  UINT64                        Cursor;
  UINT64                        BankEnd;
  UINT64                        CarveoutBase;
  UINT64                        CarveoutEnd;
  UINT64                        FdBase;
  UINT64                        FdEnd;
  UINT64                        DramSize;

  ASSERT (VirtualMemoryMap != NULL);

  if (EFI_ERROR (GetDramBanksFromSmem (Banks, &BankCount))) {
    Qcs6490Print (
      "QCS6490: No RAM partition table in SMEM, assuming %lu MiB of DRAM\n",
      FixedPcdGet64 (PcdFallbackSystemMemorySize) >> 20
      );
    Banks[0].Base = PcdGet64 (PcdSystemMemoryBase);
    Banks[0].Size = FixedPcdGet64 (PcdFallbackSystemMemorySize);
    Banks[0].Name = "DRAM";
    BankCount     = 1;
  }

  //
  // Every bank can be split once per carve-out, plus the peripheral window
  // and the terminator.
  //
  Table = AllocatePool (
            sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
            (BankCount * (ARRAY_SIZE (mCarveouts) + 1) + 2)
            );
  if (Table == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Error: Failed AllocatePool()\n", __func__));
    ASSERT (FALSE);
    return;
  }

  TableCount = 0;
  DramSize   = 0;

  DEBUG ((DEBUG_INFO, "QCS6490 memory map:\n"));

  for (BankIndex = 0; BankIndex < BankCount; BankIndex++) {
    Cursor  = Banks[BankIndex].Base;
    BankEnd = Banks[BankIndex].Base + Banks[BankIndex].Size;

    DramSize += Banks[BankIndex].Size;

    for (Index = 0; Index < ARRAY_SIZE (mCarveouts); Index++) {
      CarveoutBase = MAX (mCarveouts[Index].Base, Cursor);
      CarveoutEnd  = MIN (mCarveouts[Index].Base + mCarveouts[Index].Size, BankEnd);
      if (CarveoutBase >= CarveoutEnd) {
        continue;
      }

      if (Cursor < CarveoutBase) {
        AddDramRange (Cursor, CarveoutBase - Cursor, "DRAM", FALSE, Table, &TableCount);
      }

      AddDramRange (
        CarveoutBase,
        CarveoutEnd - CarveoutBase,
        mCarveouts[Index].Name,
        TRUE,
        Table,
        &TableCount
        );
      Cursor = CarveoutEnd;
    }

    if (Cursor < BankEnd) {
      AddDramRange (Cursor, BankEnd - Cursor, "DRAM", FALSE, Table, &TableCount);
    }
  }

  Qcs6490Print ("QCS6490: %lu MiB of DRAM\n", DramSize >> 20);

  //
  // The firmware volume XBL loaded us into is still executing. It lies in
  // DRAM, clear of every carve-out.
  //
  FdBase = PcdGet64 (PcdFdBaseAddress);
  FdEnd  = FdBase + PcdGet32 (PcdFdSize);
  for (Index = 0; Index < ARRAY_SIZE (mCarveouts); Index++) {
    ASSERT (
      FdEnd <= mCarveouts[Index].Base ||
      FdBase >= mCarveouts[Index].Base + mCarveouts[Index].Size
      );
  }

  BuildMemoryAllocationHob (FdBase, FdEnd - FdBase, EfiBootServicesData);

  //
  // Peripheral space below DRAM.
  //
  Table[TableCount].PhysicalBase = QCS6490_PERIPHERAL_BASE;
  Table[TableCount].VirtualBase  = QCS6490_PERIPHERAL_BASE;
  Table[TableCount].Length       = PcdGet64 (PcdSystemMemoryBase) - QCS6490_PERIPHERAL_BASE;
  Table[TableCount].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  TableCount++;

  // End of Table
  ZeroMem (&Table[TableCount], sizeof (ARM_MEMORY_REGION_DESCRIPTOR));

  *VirtualMemoryMap = Table;
}