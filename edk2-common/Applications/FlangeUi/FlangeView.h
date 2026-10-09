/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_VIEW_H
#define FLANGE_VIEW_H
#include <lvgl/lvgl.h>
#define FLANGE_TITLE_SIZE 256
#define FLANGE_PATH_SIZE 768
typedef struct {
  char Title[FLANGE_TITLE_SIZE];
  char Path[FLANGE_PATH_SIZE];
  uint16_t Number;
} FLANGE_ENTRY;
typedef struct {
  FLANGE_ENTRY *Entries;
  size_t Count;
  size_t Selected;
  bool Chinese;
  int SecureBoot; /* -1: unavailable, 0: off, 1: on */
  const char *Status;
} FLANGE_MODEL;
typedef enum {
  FlangeNone, FlangeBoot, FlangeSetup, FlangeRefresh, FlangeLanguage, FlangeExit, FlangeThemes
} FLANGE_ACTION;
void FlangeViewCreate(FLANGE_MODEL *Model);
void FlangeViewKey(uint32_t Key);
FLANGE_ACTION FlangeViewPoll(void);
LV_FONT_DECLARE(FlangeCjk24);
LV_FONT_DECLARE(FlangeSmall18);
LV_FONT_DECLARE(FlangeLabel14);
LV_FONT_DECLARE(FlangeBody20);
LV_FONT_DECLARE(FlangeTitle28);
LV_FONT_DECLARE(FlangeMono14);
LV_FONT_DECLARE(FlangeMono20);
#endif
