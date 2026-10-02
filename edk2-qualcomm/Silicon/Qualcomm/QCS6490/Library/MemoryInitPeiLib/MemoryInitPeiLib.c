/** @file
  MemoryInitPeiLib for QCS6490.

  Unlike the ArmPlatformPkg instance, this one does not describe DRAM itself:
  ArmPlatformGetVirtualMemoryMap() publishes the DRAM, carve-out and firmware
  volume HOBs, since only the platform library knows which parts of DRAM are
  usable. All that is left here is to turn on the MMU.

  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>

VOID
BuildMemoryTypeInformationHob (
  VOID
  );

/**
  Checks that a range lies in DRAM the platform library published as usable.

  @param[in]  Base  The start of the range.
  @param[in]  Size  The size of the range.

  @retval TRUE   The range is inside one usable DRAM resource.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
IsUsableDram (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  EFI_PEI_HOB_POINTERS  Alloc;
  EFI_PHYSICAL_ADDRESS  ResourceTop;
  EFI_PHYSICAL_ADDRESS  AllocTop;

  for (Hob.Raw = GetHobList ();
       (Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw)) != NULL;
       Hob.Raw = GET_NEXT_HOB (Hob))
  {
    ResourceTop = Hob.ResourceDescriptor->PhysicalStart +
                  Hob.ResourceDescriptor->ResourceLength;
    if ((Hob.ResourceDescriptor->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) ||
        (Base < Hob.ResourceDescriptor->PhysicalStart) ||
        (Base + Size > ResourceTop))
    {
      continue;
    }

    //
    // Carve-outs are system memory with a reserved allocation on top.
    //
    for (Alloc.Raw = GetHobList ();
         (Alloc.Raw = GetNextHob (EFI_HOB_TYPE_MEMORY_ALLOCATION, Alloc.Raw)) != NULL;
         Alloc.Raw = GET_NEXT_HOB (Alloc))
    {
      AllocTop = Alloc.MemoryAllocation->AllocDescriptor.MemoryBaseAddress +
                 Alloc.MemoryAllocation->AllocDescriptor.MemoryLength;
      if ((Alloc.MemoryAllocation->AllocDescriptor.MemoryType == EfiReservedMemoryType) &&
          (Alloc.MemoryAllocation->AllocDescriptor.MemoryBaseAddress < Base + Size) &&
          (AllocTop > Base))
      {
        return FALSE;
      }
    }

    return TRUE;
  }

  return FALSE;
}

/**
  Initializes the MMU and publishes the memory HOBs.

  @param[in]  UefiMemoryBase  The base of the permanent PEI memory.
  @param[in]  UefiMemorySize  The size of the permanent PEI memory.

  @retval EFI_SUCCESS  The MMU is on.
**/
EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  RETURN_STATUS                 Status;

  // Get Virtual Memory Map from the Platform Library
  ArmPlatformGetVirtualMemoryMap (&MemoryTable);

  //
  // MemoryInitPeim put the permanent PEI memory at the top of the window
  // PcdSystemMemoryBase/Size describes, before DRAM was known. DXE starts
  // from there, so it has to be usable DRAM.
  //
  if (!IsUsableDram (UefiMemoryBase, UefiMemorySize)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PEI memory 0x%lx-0x%lx is not usable DRAM\n",
      __func__,
      UefiMemoryBase,
      UefiMemoryBase + UefiMemorySize
      ));
    ASSERT (FALSE);
  }

  // Note: Because we called PeiServicesInstallPeiMemory() before to call InitMmu() the MMU Page Table resides in
  //      DRAM (even at the top of DRAM as it is the first permanent memory allocation)
  Status = ArmConfigureMmu (MemoryTable, NULL, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Error: Failed to enable MMU\n"));
  }

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    // Optional feature that helps prevent EFI memory map fragmentation.
    BuildMemoryTypeInformationHob ();
  }

  return EFI_SUCCESS;
}
