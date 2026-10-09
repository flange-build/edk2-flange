/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_PANEL_H
#define FLANGE_PANEL_H
#include "FlangeTheme.h"
#define FLANGE_SCAN(n) (0x10000u + (n))
#define FLANGE_SCAN_UP FLANGE_SCAN(1)
#define FLANGE_SCAN_DOWN FLANGE_SCAN(2)
#define FLANGE_SCAN_RIGHT FLANGE_SCAN(3)
#define FLANGE_SCAN_LEFT FLANGE_SCAN(4)
#define FLANGE_SCAN_HOME FLANGE_SCAN(5)
#define FLANGE_SCAN_END FLANGE_SCAN(6)
#define FLANGE_SCAN_PGUP FLANGE_SCAN(9)
#define FLANGE_SCAN_PGDN FLANGE_SCAN(10)
#define FLANGE_SCAN_ESC FLANGE_SCAN(23)
typedef struct {
  char Label[256], Value[256], Help[2048];
  bool Enabled, Group, Changed;
} FLANGE_ROW;
typedef enum { PanelNone, PanelSelect, PanelBack, PanelKey, PanelRefresh } FLANGE_PANEL_ACTION;
typedef struct {
  char Title[256], Description[2048], Footer[512];
  FLANGE_ROW *Rows;
  size_t Count, Selected;
  bool Chinese, Reorder, Raw, Password, ReadOnly;
  uint16_t *Input;
  size_t Capacity;
  uint32_t Key;
  FLANGE_PANEL_ACTION Action;
} FLANGE_PANEL;
void FlangePanelCreate(void *Context);
void FlangePanelKey(void *Context, uint32_t Key);
bool FlangePanelPoll(void *Context);
#endif
