/* Palettes from arknights-printer; shared by every screen and dialog.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FlangeTheme.h"
FLANGE_THEME FlangeTheme = FlangeField;
static const FLANGE_PALETTE Palettes[] = {
 {0xf2f2ef,0xfafaf8,0x191919,0x6e6e6a,0xc4c4be,0xfff200,0x191919,0x1c1c1c,"Field"},
 {0x0b121a,0x121c26,0xe3e8ec,0x9aaab8,0x4a5866,0x2fc6d2,0x0b121a,0x06090d,"Terminal"},
 {0xefeee0,0xf6f5ea,0x141414,0x77723c,0xc9c48e,0xcfc76e,0x141414,0x141414,"Paper"},
 {0xdcdcda,0xececea,0x161616,0x6b6b6b,0xb5b5b3,0xc8102e,0xffffff,0x232323,"Wartime"}
};
const FLANGE_PALETTE *FlangePalette(void) { return &Palettes[FlangeTheme < FlangeHii ? FlangeTheme : FlangeField]; }
