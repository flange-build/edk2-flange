/** @file
  EFI_DT_FIXUP_PROTOCOL: lets the firmware apply its fixups to a device tree
  that a loader such as GRUB is about to install.

  Specified by the U-Boot EFI project
  (https://github.com/U-Boot-EFI/EFI_DT_FIXUP_PROTOCOL); U-Boot implements it
  and GRUB calls it from its devicetree command when the firmware has it.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef EFI_DT_FIXUP_H_
#define EFI_DT_FIXUP_H_

#define EFI_DT_FIXUP_PROTOCOL_GUID \
  { 0xe617d64c, 0xfe08, 0x46da, { 0xf4, 0xdc, 0xbb, 0xd5, 0x87, 0x0c, 0x73, 0x00 } }

#define EFI_DT_FIXUP_PROTOCOL_REVISION  0x00010000

//
// Flags of Fixup().
//
#define EFI_DT_APPLY_FIXUPS    0x00000001   // apply the firmware's fixups
#define EFI_DT_RESERVE_MEMORY  0x00000002   // add the firmware's memory reservations
#define EFI_DT_INSTALL_TABLE   0x00000004   // install the result as the DTB configuration table

typedef struct _EFI_DT_FIXUP_PROTOCOL EFI_DT_FIXUP_PROTOCOL;

/**
  Applies the firmware's fixups to a device tree.

  @param[in]      This        The protocol.
  @param[in,out]  Fdt         The device tree, fixed up in place.
  @param[in,out]  BufferSize  The size of the buffer holding the device tree;
                              on EFI_BUFFER_TOO_SMALL, the size needed.
  @param[in]      Flags       EFI_DT_* flags.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  Fdt is not a valid device tree, or a flag is
                                 unknown.
  @retval EFI_BUFFER_TOO_SMALL   The buffer is too small for the result.
**/
typedef
EFI_STATUS
(EFIAPI *EFI_DT_FIXUP)(
  IN     EFI_DT_FIXUP_PROTOCOL  *This,
  IN OUT VOID                   *Fdt,
  IN OUT UINTN                  *BufferSize,
  IN     UINT32                 Flags
  );

struct _EFI_DT_FIXUP_PROTOCOL {
  UINT64          Revision;
  EFI_DT_FIXUP    Fixup;
};

extern EFI_GUID  gEfiDtFixupProtocolGuid;

#endif // EFI_DT_FIXUP_H_
