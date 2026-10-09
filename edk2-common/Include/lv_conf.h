/* LVGL 9.3.0: bounded software renderer for the boot-services environment.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_FONT_FMT_TXT_LARGE 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_MEM_SIZE (4 * 1024 * 1024U)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_USE_FLOAT 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(FlangeBody20)
#define LV_FONT_DEFAULT &FlangeBody20
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_LINE 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#ifdef FLANGE_UEFI
#define LV_STDINT_INCLUDE <FlangeStd.h>
#define LV_STDDEF_INCLUDE <FlangeStd.h>
#define LV_STDBOOL_INCLUDE <FlangeStd.h>
#define LV_INTTYPES_INCLUDE <FlangeStd.h>
#define LV_LIMITS_INCLUDE <FlangeStd.h>
#define LV_STDARG_INCLUDE <FlangeStd.h>
#endif
#define LV_USE_OBJ_ID_BUILTIN 0
#define LV_USE_OBJ_PROPERTY_NAME 0
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0
#define LV_USE_OBSERVER 0
#endif
