/** @file

  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef CJK_FONT_DXE_H_
#define CJK_FONT_DXE_H_

#include <Uefi.h>

//
// Unifont draws sixteen rows; EFI glyphs are EFI_GLYPH_HEIGHT (19) tall.
// LaffStd puts the baseline under row 14, Unifont under row 13 of its own,
// so one blank row on top lines the two fonts up and leaves two below.
//
#define CJK_GLYPH_ROWS        16
#define CJK_GLYPH_TOP_MARGIN  1

typedef struct {
  CHAR16    CodePoint;
  CHAR8     Rows[CJK_GLYPH_ROWS * 2 + 1];
} CJK_NARROW_GLYPH_HEX;

typedef struct {
  CHAR16    CodePoint;
  CHAR8     Rows[CJK_GLYPH_ROWS * 4 + 1];
} CJK_WIDE_GLYPH_HEX;

extern CONST CJK_NARROW_GLYPH_HEX  mCjkNarrowGlyphs[];
extern CONST UINTN                 mCjkNarrowGlyphCount;
extern CONST CJK_WIDE_GLYPH_HEX    mCjkWideGlyphs[];
extern CONST UINTN                 mCjkWideGlyphCount;

#endif // CJK_FONT_DXE_H_
