/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_THEME_H
#define FLANGE_THEME_H
#include "FlangeView.h"
typedef enum { FlangeField, FlangeTerminal, FlangePaper, FlangeWartime, FlangeHii, FlangeThemeCount } FLANGE_THEME;
typedef struct {
  uint32_t Ground, Surface, Ink, Muted, Rule, Brand, OnBrand, Panel;
  const char *Name;
} FLANGE_PALETTE;
extern FLANGE_THEME FlangeTheme;
const FLANGE_PALETTE *FlangePalette(void);
#endif
