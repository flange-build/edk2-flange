/** @file

  Registers a simple font package carrying the GB 2312 glyphs, so that the
  HII font code has something to draw Simplified Chinese with.

  HiiDatabaseDxe searches every simple font package for a glyph, so nothing
  has to be wired up beyond adding the package: GraphicsConsoleDxe and the
  display engine pick the glyphs up on their own. What they do need is to be
  told a character is wide, which is the job of the \wide and \narrow markers
  in the strings, not of the font.

  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "CjkFontDxe.h"

STATIC EFI_GUID  mCjkFontPackageListGuid = {
  0x56936b47, 0x8fb5, 0x4b63, { 0xb5, 0x62, 0xab, 0x0b, 0x26, 0x4e, 0x2c, 0xd9 }
};

STATIC
UINT8
HexByte (
  IN CONST CHAR8  *Hex
  )
{
  UINT8  Value;
  UINTN  Index;
  CHAR8  Digit;

  Value = 0;
  for (Index = 0; Index < 2; Index++) {
    Digit = Hex[Index];
    Value <<= 4;
    if ((Digit >= '0') && (Digit <= '9')) {
      Value |= (UINT8)(Digit - '0');
    } else if ((Digit >= 'A') && (Digit <= 'F')) {
      Value |= (UINT8)(Digit - 'A' + 10);
    } else {
      ASSERT (FALSE);
    }
  }

  return Value;
}

EFI_STATUS
EFIAPI
CjkFontDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32                           PackageLength;
  UINT8                            *Package;
  EFI_HII_SIMPLE_FONT_PACKAGE_HDR  *SimpleFont;
  EFI_NARROW_GLYPH                 *Narrow;
  EFI_WIDE_GLYPH                   *Wide;
  UINTN                            Index;
  UINTN                            Row;
  EFI_HII_HANDLE                   HiiHandle;

  ASSERT (mCjkNarrowGlyphCount <= MAX_UINT16);
  ASSERT (mCjkWideGlyphCount <= MAX_UINT16);

  //
  // HiiAddPackages() wants each package prefixed with the length of the
  // whole blob, prefix included.
  //
  PackageLength = sizeof (UINT32) +
                  sizeof (EFI_HII_SIMPLE_FONT_PACKAGE_HDR) +
                  (UINT32)(mCjkNarrowGlyphCount * sizeof (EFI_NARROW_GLYPH)) +
                  (UINT32)(mCjkWideGlyphCount * sizeof (EFI_WIDE_GLYPH));

  Package = AllocateZeroPool (PackageLength);
  if (Package == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  WriteUnaligned32 ((UINT32 *)Package, PackageLength);

  SimpleFont                       = (EFI_HII_SIMPLE_FONT_PACKAGE_HDR *)(Package + sizeof (UINT32));
  SimpleFont->Header.Length        = PackageLength - sizeof (UINT32);
  SimpleFont->Header.Type          = EFI_HII_PACKAGE_SIMPLE_FONTS;
  SimpleFont->NumberOfNarrowGlyphs = (UINT16)mCjkNarrowGlyphCount;
  SimpleFont->NumberOfWideGlyphs   = (UINT16)mCjkWideGlyphCount;

  Narrow = (EFI_NARROW_GLYPH *)(SimpleFont + 1);
  for (Index = 0; Index < mCjkNarrowGlyphCount; Index++, Narrow++) {
    Narrow->UnicodeWeight = mCjkNarrowGlyphs[Index].CodePoint;
    for (Row = 0; Row < CJK_GLYPH_ROWS; Row++) {
      Narrow->GlyphCol1[CJK_GLYPH_TOP_MARGIN + Row] = HexByte (&mCjkNarrowGlyphs[Index].Rows[Row * 2]);
    }
  }

  Wide = (EFI_WIDE_GLYPH *)Narrow;
  for (Index = 0; Index < mCjkWideGlyphCount; Index++, Wide++) {
    Wide->UnicodeWeight = mCjkWideGlyphs[Index].CodePoint;
    for (Row = 0; Row < CJK_GLYPH_ROWS; Row++) {
      Wide->GlyphCol1[CJK_GLYPH_TOP_MARGIN + Row] = HexByte (&mCjkWideGlyphs[Index].Rows[Row * 4]);
      Wide->GlyphCol2[CJK_GLYPH_TOP_MARGIN + Row] = HexByte (&mCjkWideGlyphs[Index].Rows[Row * 4 + 2]);
    }
  }

  HiiHandle = HiiAddPackages (&mCjkFontPackageListGuid, NULL, Package, NULL);
  FreePool (Package);

  if (HiiHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to register the font package\n", __func__));
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %u narrow and %u wide glyphs\n",
    __func__,
    (UINT32)mCjkNarrowGlyphCount,
    (UINT32)mCjkWideGlyphCount
    ));

  return EFI_SUCCESS;
}
