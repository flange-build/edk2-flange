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
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EfiDtFixup.h>

#include "DspPreload.h"

STATIC CONST DSP  *mFixupDsps;
STATIC UINTN      mFixupDspCount;

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
  Implements EFI_DT_FIXUP_PROTOCOL.Fixup(): removes the iommus property of
  the remoteproc nodes of the DSPs that run. The device tree only shrinks,
  in place.

  @param[in]      This        The protocol.
  @param[in,out]  Fdt         The device tree.
  @param[in,out]  BufferSize  The size of its buffer.
  @param[in]      Flags       EFI_DT_* flags.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  Not a device tree, or unknown flags.
  @retval EFI_BUFFER_TOO_SMALL   The buffer is smaller than the device tree.
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
