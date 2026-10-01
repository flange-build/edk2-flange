/** @file
  PCI Host Bridge Library for the QCS6490: PCIe0 as the one root bridge of
  PciHostBridgeDxe, on boards that set PcdPcie0Enable.

  The root complex is brought up the first time PciHostBridgeDxe asks for
  its root bridges (Pcie0Init.c). Without a link there is no root bridge,
  and PciHostBridgeDxe unloads.

  The root bridge has segment 0 and buses 0 (the root port) and 1 (the
  device on the link); the 32-bit MEM window of pcie0's ranges, where the
  PCI address is the CPU address; no IO window (IO decode would need its
  own iATU window, and no device here needs it); no 64-bit or prefetchable
  window, so PciBusDxe places 64-bit BARs in the MEM window.

  Copyright (c) 2023-2025, Mario Bălănică <mariobalanica02@gmail.com>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/PciHostBridgeLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>

#include "Pcie0Init.h"

#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH        AcpiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevicePath;
} PCIE0_ROOT_BRIDGE_DEVICE_PATH;
#pragma pack ()

//
// PciRoot(0): a PCI Express root bridge (PNP0A08) with UID 0.
//
STATIC PCIE0_ROOT_BRIDGE_DEVICE_PATH  mPcie0DevicePath = {
  {
    {
      ACPI_DEVICE_PATH,
      ACPI_DP,
      {
        (UINT8)sizeof (ACPI_HID_DEVICE_PATH),
        (UINT8)(sizeof (ACPI_HID_DEVICE_PATH) >> 8)
      }
    },
    EISA_PNP_ID (0x0A08),
    PCIE0_SEGMENT
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

STATIC PCI_ROOT_BRIDGE  mPcie0RootBridge = {
  PCIE0_SEGMENT,                                      // Segment
  0,                                                  // Supports
  0,                                                  // Attributes
  FALSE,                                              // DmaAbove4G
  FALSE,                                              // NoExtendedConfigSpace
  FALSE,                                              // ResourceAssigned
  EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM,               // AllocationAttributes
  { PCIE0_ROOT_BUS, PCIE0_SECONDARY_BUS, 0 },         // Bus
  { MAX_UINT64, 0, 0 },                               // Io
  { PCIE0_MEM_BASE, PCIE0_MEM_LIMIT, 0 },             // Mem
  { MAX_UINT64, 0, 0 },                               // MemAbove4G
  { MAX_UINT64, 0, 0 },                               // PMem
  { MAX_UINT64, 0, 0 },                               // PMemAbove4G
  (EFI_DEVICE_PATH_PROTOCOL *)&mPcie0DevicePath       // DevicePath
};

GLOBAL_REMOVE_IF_UNREFERENCED
CHAR16  *mPciHostBridgeLibAcpiAddressSpaceTypeStr[] = {
  L"Mem", L"I/O", L"Bus"
};

STATIC BOOLEAN  mPcie0Tried;
STATIC BOOLEAN  mPcie0Up;

/**
  Reads how the board wires PCIe0 from the PCDs. The power GPIO list is
  UINT16s, not necessarily aligned, ending at 0xFFFF or with the PCD.

  @param[out]  Board  How the board wires PCIe0.

  @retval EFI_SUCCESS            Read.
  @retval EFI_BAD_BUFFER_SIZE    More power GPIOs than supported.
**/
STATIC
EFI_STATUS
Pcie0ReadBoard (
  OUT PCIE0_BOARD  *Board
  )
{
  CONST UINT8  *List;
  UINTN        Count;
  UINTN        Index;
  UINT16       Gpio;

  Board->PerstGpio      = PcdGet16 (PcdPcie0PerstGpio);
  Board->PowerRampUs    = PcdGet32 (PcdPcie0PowerRampUs);
  Board->PowerGpioCount = 0;

  List  = (CONST UINT8 *)PcdGetPtr (PcdPcie0PowerGpios);
  Count = PcdGetSize (PcdPcie0PowerGpios) / sizeof (UINT16);

  for (Index = 0; Index < Count; Index++) {
    Gpio = ReadUnaligned16 ((CONST UINT16 *)(List + Index * sizeof (UINT16)));
    if (Gpio == MAX_UINT16) {
      break;
    }

    if (Board->PowerGpioCount >= PCIE0_MAX_POWER_GPIOS) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: PcdPcie0PowerGpios lists more than %u GPIOs\n",
        __func__,
        PCIE0_MAX_POWER_GPIOS
        ));
      return EFI_BAD_BUFFER_SIZE;
    }

    Board->PowerGpios[Board->PowerGpioCount++] = Gpio;
  }

  return EFI_SUCCESS;
}

/**
  Return all the root bridge instances in an array.

  PCIe0 is brought up on the first call; when it is not enabled for the
  board or its link did not come up, there is none.

  @param Count  Return the count of root bridge instances.

  @return All the root bridge instances in an array.
          The array should be passed into PciHostBridgeFreeRootBridges()
          when it's not used.
**/
PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  PCIE0_BOARD  Board;
  EFI_STATUS   Status;

  if (!mPcie0Tried) {
    mPcie0Tried = TRUE;

    if (!PcdGetBool (PcdPcie0Enable)) {
      DEBUG ((DEBUG_INFO, "%a: PCIe0 is not enabled on this board\n", __func__));
    } else {
      Status = Pcie0ReadBoard (&Board);
      if (!EFI_ERROR (Status)) {
        Status = Qcs6490Pcie0Init (&Board);
      }

      mPcie0Up = !EFI_ERROR (Status);
    }
  }

  if (!mPcie0Up) {
    *Count = 0;
    return NULL;
  }

  *Count = 1;
  return &mPcie0RootBridge;
}

/**
  Free the root bridge instances array returned from PciHostBridgeGetRootBridges().

  The array is static: there is nothing to free.

  @param Bridges The root bridge instances array.
  @param Count   The count of the array.
**/
VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
}

/**
  Inform the platform that the resource conflict happens.

  @param HostBridgeHandle Handle of the Host Bridge.
  @param Configuration    Pointer to PCI I/O and PCI memory resource
                          descriptors. The Configuration contains the resources
                          for all the root bridges. The resource for each root
                          bridge is terminated with END descriptor and an
                          additional END is appended indicating the end of the
                          entire resources. The resource descriptor field
                          values follow the description in
                          EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL
                          .SubmitResources().
**/
VOID
EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  HostBridgeHandle,
  VOID        *Configuration
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Descriptor;
  UINTN                              RootBridgeIndex;

  DEBUG ((DEBUG_ERROR, "PciHostBridge: Resource conflict happens!\n"));

  RootBridgeIndex = 0;
  Descriptor      = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Configuration;
  while (Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) {
    DEBUG ((DEBUG_ERROR, "RootBridge[%u]:\n", (UINT32)RootBridgeIndex));
    RootBridgeIndex++;
    for ( ; Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR; Descriptor++) {
      ASSERT (
        Descriptor->ResType <
        (sizeof (mPciHostBridgeLibAcpiAddressSpaceTypeStr) /
         sizeof (mPciHostBridgeLibAcpiAddressSpaceTypeStr[0])
        )
        );
      DEBUG ((
        DEBUG_ERROR,
        " %s: Length/Alignment = 0x%lx / 0x%lx\n",
        mPciHostBridgeLibAcpiAddressSpaceTypeStr[Descriptor->ResType],
        Descriptor->AddrLen,
        Descriptor->AddrRangeMax
        ));
      if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        DEBUG ((
          DEBUG_ERROR,
          "     Granularity/SpecificFlag = %ld / %02x%s\n",
          Descriptor->AddrSpaceGranularity,
          Descriptor->SpecificFlag,
          ((Descriptor->SpecificFlag &
            EFI_ACPI_MEMORY_RESOURCE_SPECIFIC_FLAG_CACHEABLE_PREFETCHABLE
            ) != 0) ? L" (Prefetchable)" : L""
          ));
      }
    }

    //
    // Skip the END descriptor for root bridge
    //
    ASSERT (Descriptor->Desc == ACPI_END_TAG_DESCRIPTOR);
    Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(
                                                       (EFI_ACPI_END_TAG_DESCRIPTOR *)Descriptor + 1
                                                       );
  }
}
