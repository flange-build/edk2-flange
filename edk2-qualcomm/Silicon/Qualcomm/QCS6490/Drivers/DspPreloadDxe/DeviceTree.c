/** @file
  The device tree side of the DSP preload.

  The board's device tree (the one DtPlatformDxe installs) names each DSP's
  firmware and the memory it runs from. When a DSP runs, the device tree the
  OS gets must not give its remoteproc node an iommus property: Linux would
  attach the node to an SMMU domain that maps nothing, and the DSP's next
  memory access would fault. flange's EL2 device trees have that property,
  for a Linux that loads the DSPs itself. GRUB, which loads the device tree,
  hands it to EFI_DT_FIXUP_PROTOCOL first; the fixup removes the property
  from the nodes of the DSPs UEFI started.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/Fdt.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EfiDtFixup.h>
#include <Protocol/GraphicsOutput.h>

#include "DspPreload.h"

STATIC CONST DSP  *mFixupDsps;
STATIC UINTN      mFixupDspCount;

/**
  Fill the board-specific framebuffer template from GOP. Clone its resource
  properties into a correctly named runtime node, keeping clocks and supplies
  attached to the framebuffer rather than an unclaimed native display device.
  No template means the loader supplied a different display configuration.
**/
STATIC
EFI_STATUS
DtFixupDisplay (
  IN OUT VOID   *Fdt,
  IN OUT UINTN  *BufferSize
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   *Info;
  EFI_STATUS                            Status;
  VOID                                  *Original;
  INT32                                 Node;
  INT32                                 NewNode;
  INT32                                 Chosen;
  INT32                                 Property;
  INT32                                 Length;
  INT32                                 Error;
  CONST FDT_PROPERTY                     *Prop;
  CONST CHAR8                           *Name;
  CONST UINT32                          *Reg;
  UINT32                                Cells[4];
  CHAR8                                 NodeName[40];

  if (!FixedPcdGetBool (PcdDisplayHandoff)) {
    return EFI_SUCCESS;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  Node   = FdtNodeOffsetByCompatible (Fdt, -1, "simple-framebuffer");
  if ((Node < 0) || (FdtParentOffset (Fdt, Node) != Chosen)) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL) ||
      (Gop->Mode->FrameBufferSize == 0))
  {
    // An incomplete template must not suppress Linux's EFI framebuffer.
    FdtDelNode (Fdt, Node);
    return EFI_SUCCESS;
  }

  Mode = Gop->Mode;
  Info = Mode->Info;
  if ((Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) ||
      (Mode->FrameBufferBase == 0))
  {
    return EFI_UNSUPPORTED;
  }

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Reg != NULL) && (Length == sizeof (Cells)) &&
      (Fdt32ToCpu (Reg[0]) == (UINT32)RShiftU64 (Mode->FrameBufferBase, 32)) &&
      (Fdt32ToCpu (Reg[1]) == (UINT32)Mode->FrameBufferBase))
  {
    return EFI_SUCCESS;
  }

  if (*BufferSize < FdtTotalSize (Fdt) + SIZE_1KB) {
    *BufferSize = FdtTotalSize (Fdt) + SIZE_1KB;
    return EFI_BUFFER_TOO_SMALL;
  }

  Original = AllocateCopyPool (FdtTotalSize (Fdt), Fdt);
  if (Original == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Error = FdtOpenInto (Fdt, Fdt, (INT32)*BufferSize);
  if (Error != 0) {
    FreePool (Original);
    return EFI_DEVICE_ERROR;
  }

  Error = FdtDelNode (Fdt, Node);
  AsciiSPrint (NodeName, sizeof (NodeName), "framebuffer@%lx", Mode->FrameBufferBase);
  NewNode = (Error == 0) ? FdtAddSubnode (Fdt, Chosen, NodeName) : Error;
  if (NewNode < 0) {
    FreePool (Original);
    return EFI_DEVICE_ERROR;
  }

  for (Property = FdtFirstPropertyOffset (Original, Node);
       Property >= 0;
       Property = FdtNextPropertyOffset (Original, Property))
  {
    Prop = FdtGetPropertyByOffset (Original, Property, &Length);
    if (Prop == NULL) {
      Error = Length;
      break;
    }

    Name  = FdtGetString (Original, Fdt32ToCpu (Prop->NameOffset), NULL);
    Error = FdtSetProp (Fdt, NewNode, Name, Prop->Data, Fdt32ToCpu (Prop->Length));
    if (Error != 0) {
      break;
    }
  }

  FreePool (Original);
  Cells[0] = CpuToFdt32 ((UINT32)RShiftU64 (Mode->FrameBufferBase, 32));
  Cells[1] = CpuToFdt32 ((UINT32)Mode->FrameBufferBase);
  Cells[2] = CpuToFdt32 ((UINT32)RShiftU64 (Mode->FrameBufferSize, 32));
  Cells[3] = CpuToFdt32 ((UINT32)Mode->FrameBufferSize);
  if (Error == 0) {
    Error = FdtSetProp (Fdt, NewNode, "reg", Cells, sizeof (Cells));
  }

  Cells[0] = CpuToFdt32 (Info->HorizontalResolution);
  if (Error == 0) {
    Error = FdtSetProp (Fdt, NewNode, "width", Cells, sizeof (UINT32));
  }

  Cells[0] = CpuToFdt32 (Info->VerticalResolution);
  if (Error == 0) {
    Error = FdtSetProp (Fdt, NewNode, "height", Cells, sizeof (UINT32));
  }

  Cells[0] = CpuToFdt32 (Info->PixelsPerScanLine * sizeof (UINT32));
  if (Error == 0) {
    Error = FdtSetProp (Fdt, NewNode, "stride", Cells, sizeof (UINT32));
  }

  if (Error == 0) {
    Error = FdtSetPropString (Fdt, NewNode, "status", "okay");
  }

  if (Error != 0) {
    FdtDelNode (Fdt, NewNode);
    DEBUG ((DEBUG_ERROR, "%a: framebuffer fixup failed: %d\n", __func__, Error));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "%a: %a %ux%u stride %u\n", __func__, NodeName,
          Info->HorizontalResolution, Info->VerticalResolution, Info->PixelsPerScanLine * 4));
  return EFI_SUCCESS;
}

VOID
DtPrepareDisplay (
  VOID
  )
{
  VOID                  *Fdt;
  EFI_PHYSICAL_ADDRESS  Base;
  UINTN                 Size;
  EFI_STATUS            Status;

  if (!FixedPcdGetBool (PcdDisplayHandoff) ||
      EFI_ERROR (EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt)) ||
      (Fdt == NULL) || (FdtCheckHeader (Fdt) != 0))
  {
    return;
  }

  Size   = ALIGN_VALUE (FdtTotalSize (Fdt) + SIZE_4KB, EFI_PAGE_SIZE);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, EFI_SIZE_TO_PAGES (Size), &Base);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: DT allocation: %r\n", __func__, Status));
    return;
  }

  if (FdtOpenInto (Fdt, (VOID *)(UINTN)Base, (INT32)(Size - SIZE_1KB)) != 0) {
    Status = EFI_DEVICE_ERROR;
  } else {
    Status = DtFixupDisplay ((VOID *)(UINTN)Base, &Size);
    if (!EFI_ERROR (Status)) {
      Status = gBS->InstallConfigurationTable (&gFdtTableGuid, (VOID *)(UINTN)Base);
    }
  }

  if (EFI_ERROR (Status)) {
    gBS->FreePages (Base, EFI_SIZE_TO_PAGES (Size));
    DEBUG ((DEBUG_ERROR, "%a: DT handoff: %r\n", __func__, Status));
  }
}

/**
  Returns the board's device tree.

  @return  The device tree, or NULL.
**/
STATIC
VOID *
DtGetBoardFdt (
  VOID
  )
{
  VOID  *Fdt;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt)) || (Fdt == NULL) ||
      (FdtCheckHeader (Fdt) != 0))
  {
    return NULL;
  }

  return Fdt;
}

/**
  Reads the first address and size of a node's reg property.

  @param[in]   Fdt   The device tree.
  @param[in]   Node  The node.
  @param[out]  Base  The address.
  @param[out]  Size  The size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  No usable reg property.
**/
STATIC
EFI_STATUS
DtGetReg (
  IN  CONST VOID            *Fdt,
  IN  INT32                 Node,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  )
{
  CONST UINT32  *Reg;
  INT32         Length;
  INT32         Parent;
  INT32         AddressCells;
  INT32         SizeCells;
  INT32         Index;

  Parent = FdtParentOffset (Fdt, Node);
  if (Parent < 0) {
    return EFI_NOT_FOUND;
  }

  AddressCells = FdtAddressCells (Fdt, Parent);
  SizeCells    = FdtSizeCells (Fdt, Parent);
  if ((AddressCells < 1) || (AddressCells > 2) || (SizeCells < 1) || (SizeCells > 2)) {
    return EFI_NOT_FOUND;
  }

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Reg == NULL) || (Length < (AddressCells + SizeCells) * (INT32)sizeof (UINT32))) {
    return EFI_NOT_FOUND;
  }

  *Base = 0;
  for (Index = 0; Index < AddressCells; Index++) {
    *Base = LShiftU64 (*Base, 32) | Fdt32ToCpu (Reg[Index]);
  }

  *Size = 0;
  for (Index = 0; Index < SizeCells; Index++) {
    *Size = LShiftU64 (*Size, 32) | Fdt32ToCpu (Reg[AddressCells + Index]);
  }

  return EFI_SUCCESS;
}

/**
  Returns whether a node is enabled.

  @param[in]  Fdt   The device tree.
  @param[in]  Node  The node.

  @return  TRUE if it has no status, or "okay" or "ok".
**/
STATIC
BOOLEAN
DtNodeEnabled (
  IN CONST VOID  *Fdt,
  IN INT32       Node
  )
{
  CONST CHAR8  *Status;
  INT32        Length;

  Status = FdtGetProp (Fdt, Node, "status", &Length);
  if ((Status == NULL) || (Length <= 0)) {
    return TRUE;
  }

  return (AsciiStrnCmp (Status, "okay", Length) == 0) || (AsciiStrnCmp (Status, "ok", Length) == 0);
}

EFI_STATUS
DtGetDsp (
  IN OUT DSP  *Dsp
  )
{
  VOID          *Fdt;
  INT32         Node;
  INT32         Region;
  INT32         Length;
  CONST CHAR8   *Name;
  CONST UINT32  *Phandle;

  Fdt = DtGetBoardFdt ();
  if (Fdt == NULL) {
    return EFI_NOT_FOUND;
  }

  Node = FdtNodeOffsetByCompatible (Fdt, -1, Dsp->Desc->Compatible);
  if ((Node < 0) || !DtNodeEnabled (Fdt, Node)) {
    return EFI_NOT_FOUND;
  }

  Name = FdtGetProp (Fdt, Node, "firmware-name", &Length);
  if ((Name == NULL) || (Length <= 1) || (Name[Length - 1] != '\0') ||
      ((UINTN)Length > sizeof (Dsp->FirmwareName)))
  {
    DEBUG ((DEBUG_ERROR, "%a: %a: no usable firmware-name\n", __func__, Dsp->Desc->Name));
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Dsp->FirmwareName, Name, Length);

  Phandle = FdtGetProp (Fdt, Node, "memory-region", &Length);
  if ((Phandle == NULL) || (Length < (INT32)sizeof (UINT32))) {
    DEBUG ((DEBUG_ERROR, "%a: %a: no memory-region\n", __func__, Dsp->Desc->Name));
    return EFI_INVALID_PARAMETER;
  }

  Region = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Phandle[0]));
  if ((Region < 0) ||
      EFI_ERROR (DtGetReg (Fdt, Region, &Dsp->CarveoutBase, &Dsp->CarveoutSize)) ||
      (Dsp->CarveoutSize == 0) ||
      ((Dsp->CarveoutBase & EFI_PAGE_MASK) != 0) || ((Dsp->CarveoutSize & EFI_PAGE_MASK) != 0))
  {
    DEBUG ((DEBUG_ERROR, "%a: %a: no usable memory-region\n", __func__, Dsp->Desc->Name));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
DtGetCmdDb (
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  )
{
  VOID   *Fdt;
  INT32  Node;

  Fdt = DtGetBoardFdt ();
  if (Fdt == NULL) {
    return EFI_NOT_FOUND;
  }

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "qcom,cmd-db");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  return DtGetReg (Fdt, Node, Base, Size);
}

UINTN
DtFixupRunningDsps (
  IN VOID       *Fdt,
  IN CONST DSP  *Dsps,
  IN UINTN      Count
  )
{
  UINTN  Index;
  UINTN  Removed;
  INT32  Node;
  INT32  Length;

  Removed = 0;
  for (Index = 0; Index < Count; Index++) {
    if (Dsps[Index].State != DspStateRunning) {
      continue;
    }

    Node = FdtNodeOffsetByCompatible (Fdt, -1, Dsps[Index].Desc->Compatible);
    if ((Node < 0) || (FdtGetProp (Fdt, Node, "iommus", &Length) == NULL)) {
      continue;
    }

    if (FdtDelProp (Fdt, Node, "iommus") == 0) {
      DEBUG ((DEBUG_INFO, "%a: removed iommus of the %a remoteproc\n", __func__, Dsps[Index].Desc->Name));
      Removed++;
    } else {
      DEBUG ((DEBUG_ERROR, "%a: cannot remove iommus of the %a remoteproc\n", __func__, Dsps[Index].Desc->Name));
    }
  }

  return Removed;
}

/**
  Implements EFI_DT_FIXUP_PROTOCOL.Fixup(): updates the framebuffer template
  and removes iommus from running DSPs. The framebuffer fixup may require
  more space; report the required size before modifying the caller's tree.

  @param[in]      This        The protocol.
  @param[in,out]  Fdt         The device tree.
  @param[in,out]  BufferSize  The size of its buffer.
  @param[in]      Flags       EFI_DT_* flags.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  Not a device tree, or unknown flags.
  @retval EFI_BUFFER_TOO_SMALL   More buffer space is needed for the fixup.
**/
STATIC
EFI_STATUS
EFIAPI
DtFixup (
  IN     EFI_DT_FIXUP_PROTOCOL  *This,
  IN OUT VOID                   *Fdt,
  IN OUT UINTN                  *BufferSize,
  IN     UINT32                 Flags
  )
{
  EFI_STATUS  Status;
  UINTN       Removed;

  if ((Fdt == NULL) || (BufferSize == NULL) ||
      ((Flags & ~(UINT32)(EFI_DT_APPLY_FIXUPS | EFI_DT_RESERVE_MEMORY | EFI_DT_INSTALL_TABLE)) != 0) ||
      (FdtCheckHeader (Fdt) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (*BufferSize < FdtTotalSize (Fdt)) {
    *BufferSize = FdtTotalSize (Fdt);
    return EFI_BUFFER_TOO_SMALL;
  }

  if ((Flags & EFI_DT_APPLY_FIXUPS) != 0) {
    Status = DtFixupDisplay (Fdt, BufferSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Removed = DtFixupRunningDsps (Fdt, mFixupDsps, mFixupDspCount);
    DEBUG ((DEBUG_INFO, "%a: %u iommus properties removed\n", __func__, (UINT32)Removed));
  }

  //
  // EFI_DT_RESERVE_MEMORY needs nothing: the UEFI memory map already
  // reserves the board's carve-outs.
  //

  if ((Flags & EFI_DT_INSTALL_TABLE) != 0) {
    Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_DT_FIXUP_PROTOCOL  mDtFixupProtocol = {
  EFI_DT_FIXUP_PROTOCOL_REVISION,
  DtFixup
};

EFI_STATUS
DtInstallFixupProtocol (
  IN CONST DSP  *Dsps,
  IN UINTN      Count
  )
{
  EFI_HANDLE  Handle;

  mFixupDsps     = Dsps;
  mFixupDspCount = Count;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiDtFixupProtocolGuid,
                &mDtFixupProtocol,
                NULL
                );
}
