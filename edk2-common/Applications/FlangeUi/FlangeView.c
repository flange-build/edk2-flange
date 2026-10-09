/* FLANGE / boot control. Original industrial field-console design.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FlangeView.h"
#include "FlangeTheme.h"
#define GROUND  (FlangePalette()->Ground)
#define SURFACE (FlangePalette()->Surface)
#define INK     (FlangePalette()->Ink)
#define MUTED   (FlangePalette()->Muted)
#define RULE    (FlangePalette()->Rule)
#define BRAND   (FlangePalette()->Brand)
#define ONBRAND (FlangePalette()->OnBrand)
static FLANGE_MODEL *mModel;
static FLANGE_ACTION mAction;
static bool mDirty;
static int32_t mRows;
static const char *Tr(const char *En, const char *Zh) { return mModel->Chinese ? Zh : En; }

static lv_obj_t *Box(lv_obj_t *Parent, int32_t X, int32_t Y, int32_t W, int32_t H, uint32_t Color)
{
  lv_obj_t *Obj = lv_obj_create(Parent);
  lv_obj_remove_style_all(Obj);
  lv_obj_set_pos(Obj, X, Y);
  lv_obj_set_size(Obj, W, H);
  lv_obj_set_style_bg_color(Obj, lv_color_hex(Color), 0);
  lv_obj_set_style_bg_opa(Obj, LV_OPA_COVER, 0);
  lv_obj_remove_flag(Obj, LV_OBJ_FLAG_SCROLLABLE);
  return Obj;
}
static lv_obj_t *Text(lv_obj_t *Parent, int32_t X, int32_t Y, int32_t W,
                      const char *Value, uint32_t Color, const lv_font_t *Font)
{
  lv_obj_t *Obj = lv_label_create(Parent);
  lv_obj_set_pos(Obj, X, Y);
  lv_obj_set_width(Obj, W);
  lv_obj_set_style_text_font(Obj, Font, 0);
  lv_obj_set_style_text_color(Obj, lv_color_hex(Color), 0);
  lv_label_set_text(Obj, Value);
  if(Font == &FlangeCjk24 || Font == &FlangeSmall18) lv_obj_set_style_text_letter_space(Obj, 1, 0);
  if(Font == &FlangeLabel14) lv_obj_set_style_text_letter_space(Obj, 2, 0);
  lv_obj_remove_flag(Obj, LV_OBJ_FLAG_CLICKABLE);
  return Obj;
}
static void ActionClick(lv_event_t *Event)
{
  mAction = (FLANGE_ACTION)(uintptr_t)lv_event_get_user_data(Event);
}
static void PageClick(lv_event_t *Event)
{
  FlangeViewKey((uint32_t)(uintptr_t)lv_event_get_user_data(Event));
}
static void RowClick(lv_event_t *Event)
{
  mModel->Selected = (size_t)(uintptr_t)lv_event_get_user_data(Event);
  mDirty = true;
}
static void Button(lv_obj_t *Parent, int32_t X, int32_t Y, int32_t W,
                   const char *Label, FLANGE_ACTION Action, bool Primary)
{
  lv_obj_t *Obj = Box(Parent, X, Y, W, 48, Primary ? BRAND : SURFACE);
  lv_obj_add_flag(Obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_width(Obj, 1, 0);
  lv_obj_set_style_border_color(Obj, lv_color_hex(Primary ? BRAND : RULE), 0);
  lv_obj_set_style_bg_color(Obj, lv_color_hex(RULE), LV_STATE_PRESSED);
  lv_obj_add_event_cb(Obj, ActionClick, LV_EVENT_CLICKED, (void *)(uintptr_t)Action);
  lv_obj_t *LabelObj = Text(Obj, 8, 12, W - 16, Label, Primary ? ONBRAND : INK, &FlangeSmall18);
  lv_obj_set_style_text_align(LabelObj, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_letter_space(LabelObj, 0, 0);
}
/* Two opposed brackets: flange coupling / the handoff between firmware and loader.
 * The echo is deliberately defocused; dots and live selection remain sharp. */
static void Bracket(lv_obj_t *Parent, int32_t W, int32_t H, uint32_t Color)
{
  Box(Parent, 0, 0, 12, 2, Color);
  Box(Parent, 0, 0, 2, 16, Color);
  Box(Parent, W - 12, H - 2, 12, 2, Color);
  Box(Parent, W - 2, H - 16, 2, 16, Color);
}
static void Grid(lv_event_t *Event)
{
  lv_layer_t *Layer = lv_event_get_layer(Event);
  lv_draw_rect_dsc_t Dot;
  lv_draw_rect_dsc_init(&Dot);
  int32_t W = lv_display_get_horizontal_resolution(NULL);
  int32_t H = lv_display_get_vertical_resolution(NULL);
  /* Neutral, softly layered echo of the coupling emblem. No surface glow. */
  for(int32_t Spread = 32; Spread >= 0; Spread -= 4) {
    Dot.bg_color = lv_color_hex(RULE);
    Dot.bg_opa = 4;
    lv_area_t Echo = {W * 3 / 5 - Spread, H / 7 - Spread,
                      W * 3 / 5 + 18 + Spread, H * 6 / 7 + Spread};
    lv_draw_rect(Layer, &Dot, &Echo);
    Echo.x1 = W * 3 / 5 - Spread; Echo.x2 = W * 4 / 5 + Spread;
    Echo.y1 = H / 7 - Spread; Echo.y2 = H / 7 + 18 + Spread;
    lv_draw_rect(Layer, &Dot, &Echo);
    Echo.x1 = W * 4 / 5 - Spread; Echo.x2 = W * 4 / 5 + 18 + Spread;
    Echo.y1 = H / 3 - Spread; Echo.y2 = H * 6 / 7 + Spread;
    lv_draw_rect(Layer, &Dot, &Echo);
  }
  /* A single curved link depicts the selected entry's handoff to its detail
   * plate. Its endpoints come from the live selection, never fabricated data. */
  if(mModel->Count && mRows > 0) {
    int32_t Margin = W / 20;
    int32_t Left = (W - 2 * Margin) * 55 / 100;
    int32_t Top = H >= 800 ? 210 : 164;
    int32_t RowHeight = H >= 800 ? 76 : 64;
    int32_t X = Margin + Left;
    int32_t Y = Top + (int32_t)(mModel->Selected % (size_t)mRows) * RowHeight + RowHeight / 2;
    lv_draw_line_dsc_t Link;
    lv_draw_line_dsc_init(&Link);
    Link.color = lv_color_hex(RULE);
    Link.width = 1;
    Link.p1.x = X; Link.p1.y = Y;
    for(int32_t Step = 1; Step <= 16; Step++) {
      /* Smoothstep cubic in fixed-point; compatible with -mgeneral-regs-only. */
      int32_t T = Step * 16;
      int32_t Smooth = T * T * (768 - 2 * T) / 65536;
      Link.p2.x = X + 24 * T / 256;
      Link.p2.y = Y + (Top + 72 - Y) * Smooth / 256;
      lv_draw_line(Layer, &Link);
      Link.p1 = Link.p2;
    }
  }
  if(FlangeTheme==FlangeTerminal) { Dot.bg_color=lv_color_hex(RULE); Dot.bg_opa=6;
    for(int y=0;y<H;y+=4) { lv_area_t A={0,y,W-1,y}; lv_draw_rect(Layer,&Dot,&A); }
  }
  Dot.bg_color = lv_color_hex(MUTED);
  Dot.bg_opa = 22;
  for(int32_t Y = 4; Y < H; Y += 24) {
    for(int32_t X = 4; X < W; X += 24) {
      lv_area_t Area = {X, Y, X, Y};
      lv_draw_rect(Layer, &Dot, &Area);
    }
  }
}
static void Render(void)
{
  lv_obj_t *Screen = lv_screen_active();
  lv_obj_clean(Screen);
  int32_t W = lv_display_get_horizontal_resolution(NULL);
  int32_t H = lv_display_get_vertical_resolution(NULL);
  int32_t Margin = W / 20;
  int32_t Content = W - 2 * Margin;
  int32_t Gap = 24;
  int32_t Left = Content * 55 / 100;
  int32_t Right = Content - Left - Gap;
  int32_t Top = H >= 800 ? 210 : 164;
  int32_t RowHeight = H >= 800 ? 76 : 64;
  mRows = (H - Top - 170) / RowHeight;
  if(mRows > 7) mRows = 7;
  if(mRows < 1) mRows = 1;
  int32_t BodyH = mRows * RowHeight;
  char Buffer[128];
  lv_obj_t *Emblem = Box(Screen, Margin, 34, 34, 30, GROUND);
  Bracket(Emblem, 34, 30, INK);
  if(FlangeTheme==FlangeWartime) { Box(Screen,Margin+44,26,Content-44,48,FlangePalette()->Panel); Box(Screen,Margin+44,26,4,48,BRAND); }
  Text(Screen, Margin + 52, 38, Content / 2 - 52, "FLANGE  /  FIRMWARE", FlangeTheme==FlangeWartime?0xffffff:INK, FlangeTheme==FlangeTerminal?&FlangeMono20:&FlangeBody20);
  lv_snprintf(Buffer, sizeof(Buffer), "%d x %d  /  UEFI", (int)W, (int)H);
  lv_obj_t *Mode = Text(Screen, Margin + Content / 2, 42, Content / 2, Buffer, FlangeTheme==FlangeWartime?0xffffff:MUTED, &FlangeMono14);
  lv_obj_set_style_text_align(Mode, LV_TEXT_ALIGN_RIGHT, 0);
  Box(Screen, Margin, H < 800 ? 76 : 84, Content, 1, RULE);
  Button(Screen, Margin + Left + Gap, Top - 72, Right, Tr("T / Menu theme", "T / 菜单主题"), FlangeThemes, false);
  Text(Screen, Margin, Top - 80, Left, Tr("Boot control", "启动控制"), INK, mModel->Chinese ? &FlangeCjk24 : &FlangeTitle28);
  lv_snprintf(Buffer, sizeof(Buffer), "%s  /  %u", Tr("BOOT OPTIONS", "启动项"), (unsigned)mModel->Count);
  Text(Screen, Margin, Top - 32, Left, Buffer, MUTED, &FlangeSmall18);
  size_t Page = mModel->Count ? mModel->Selected / (size_t)mRows : 0;
  size_t Start = Page * (size_t)mRows;
  for(size_t Index = Start; Index < mModel->Count && Index < Start + (size_t)mRows; Index++) {
    bool Selected = Index == mModel->Selected;
    int32_t Y = Top + (int32_t)(Index - Start) * RowHeight;
    lv_obj_t *Row = Box(Screen, Margin, Y, Left, RowHeight - 6, Selected ? BRAND : SURFACE);
    lv_obj_add_flag(Row, LV_OBJ_FLAG_CLICKABLE);
    if(Selected) Bracket(Row, Left, RowHeight - 6, ONBRAND);
    lv_obj_add_event_cb(Row, RowClick, LV_EVENT_CLICKED, (void *)(uintptr_t)Index);
    lv_snprintf(Buffer, sizeof(Buffer), "%02u", (unsigned)(Index + 1));
    Text(Row, 18, (RowHeight - 30) / 2, 42, Buffer, Selected ? ONBRAND : MUTED, &FlangeMono20);
    lv_obj_t *Title = Text(Row, 72, (RowHeight - 32) / 2, Left - 104, mModel->Entries[Index].Title, Selected ? ONBRAND : INK, &FlangeCjk24);
    lv_obj_set_height(Title, 31);
    lv_label_set_long_mode(Title, LV_LABEL_LONG_DOT);
  }
  if(!mModel->Count) {
    Text(Screen, Margin + 20, Top + 42, Left - 40, Tr("No active boot options.\nConnect media, then refresh.", "未找到可用启动项\n连接启动介质后刷新"), INK, &FlangeCjk24);
  }
  lv_obj_t *Details = Box(Screen, Margin + Left + Gap, Top, Right, BodyH - 6, SURFACE);
  Bracket(Details, Right, BodyH - 6, RULE);
  Text(Details, 24, 18, Right - 48, Tr("SELECTED TARGET", "所选目标"), MUTED, &FlangeSmall18);
  if(mModel->Count) {
    FLANGE_ENTRY *Entry = &mModel->Entries[mModel->Selected];
    lv_snprintf(Buffer, sizeof(Buffer), "Boot%04X", Entry->Number);
    Text(Details, 24, 56, Right - 48, Buffer, INK, &FlangeMono20);
    Box(Details, 24, 105, Right - 48, 1, RULE);
    lv_obj_t *Title = Text(Details, 24, 126, Right - 48, Entry->Title, INK, &FlangeCjk24);
    lv_obj_set_height(Title, 54);
    lv_label_set_long_mode(Title, LV_LABEL_LONG_DOT);
    if(BodyH >= 330) {
      Text(Details, 24, 204, Right - 48, Tr("DEVICE PATH", "设备路径"), MUTED, mModel->Chinese ? &FlangeSmall18 : &FlangeLabel14);
      lv_obj_t *Path = Text(Details, 24, 242, Right - 48, Entry->Path, MUTED, &FlangeMono14);
      lv_obj_set_height(Path, BodyH - 322);
      lv_label_set_long_mode(Path, LV_LABEL_LONG_DOT);
    }
    Button(Details, 24, BodyH - 72, Right - 48, Tr("Enter  /  Boot once", "Enter  /  启动一次"), FlangeBoot, true);
  }
  int32_t Bottom = Top + BodyH + 14;
  lv_snprintf(Buffer, sizeof(Buffer), "%s %u / %u", Tr("PAGE", "页"), (unsigned)(Page + 1), (unsigned)(mModel->Count ? (mModel->Count + mRows - 1) / mRows : 1));
  Text(Screen, Margin, Bottom, Left / 2, Buffer, MUTED, &FlangeSmall18);
  if(mModel->Count > (size_t)mRows) {
    for(int I = 0; I < 2; I++) {
      lv_obj_t *Nav = Box(Screen, Margin + Left - 92 + I * 48, Bottom - 4, 40, 32, SURFACE);
      lv_obj_add_flag(Nav, LV_OBJ_FLAG_CLICKABLE);
      Text(Nav, 14, 6, 26, I ? ">" : "<", INK, &FlangeMono20);
      lv_obj_add_event_cb(Nav, PageClick, LV_EVENT_CLICKED, (void *)(uintptr_t)(I ? LV_KEY_NEXT : LV_KEY_PREV));
    }
  }
  const char *Secure = mModel->SecureBoot < 0 ? Tr("Secure Boot: unknown", "安全启动：未知") : mModel->SecureBoot ? Tr("Secure Boot: on", "安全启动：开启") : Tr("Secure Boot: off", "安全启动：关闭");
  Text(Screen, Margin + Left + Gap, Bottom, Right, Secure, MUTED, &FlangeSmall18);
  int32_t Bw = (Content - 3 * 12) / 4;
  Button(Screen, Margin, H - 98, Bw, Tr("F2  /  Settings", "F2  /  固件设置"), FlangeSetup, false);
  Button(Screen, Margin + Bw + 12, H - 98, Bw, Tr("R  /  Refresh", "R  /  刷新"), FlangeRefresh, false);
  Button(Screen, Margin + 2 * (Bw + 12), H - 98, Bw, mModel->Chinese ? "L  /  English" : "L  /  简体中文", FlangeLanguage, false);
  Button(Screen, Margin + 3 * (Bw + 12), H - 98, Bw, Tr("Esc  /  Continue", "Esc  /  继续启动"), FlangeExit, false);
  Text(Screen, Margin, H - 36, Content, mModel->Status ? mModel->Status : Tr("UP / DOWN  SELECT     PGUP / PGDN  PAGE     ENTER  BOOT", "上下键 选择    PageUp / PageDown 翻页    Enter 启动"), MUTED, &FlangeSmall18);
}
void FlangeViewCreate(FLANGE_MODEL *Model)
{
  mModel = Model;
  mAction = FlangeNone;
  mDirty = false;
  if(!Model->Count || Model->Selected >= Model->Count) Model->Selected = 0;
  lv_obj_t *Screen = lv_screen_active();
  lv_obj_remove_style_all(Screen);
  lv_obj_set_style_bg_color(Screen, lv_color_hex(GROUND), 0);
  lv_obj_set_style_bg_opa(Screen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(Screen, LV_OBJ_FLAG_SCROLLABLE);
  while(lv_obj_get_event_count(Screen)) lv_obj_remove_event(Screen, 0);
  lv_obj_add_event_cb(Screen, Grid, LV_EVENT_DRAW_MAIN, NULL);
  Render();
}
void FlangeViewKey(uint32_t Key)
{
  /* UEFI SimpleTextInput reports CR; LVGL keyboard events use LF. */
  if((Key == '\r' || Key == LV_KEY_ENTER) && mModel->Count) mAction = FlangeBoot;
  else if(Key == LV_KEY_ESC) mAction = FlangeExit;
  else if(Key == 't' || Key == 'T') mAction = FlangeThemes;
  else if(Key == 's') mAction = FlangeSetup;
  else if(Key == 'r' || Key == 'R') mAction = FlangeRefresh;
  else if(Key == 'l' || Key == 'L') mAction = FlangeLanguage;
  else if(mModel->Count) {
    size_t Old = mModel->Selected;
    if(Key == LV_KEY_DOWN) mModel->Selected = (Old + 1) % mModel->Count;
    if(Key == LV_KEY_UP) mModel->Selected = Old ? Old - 1 : mModel->Count - 1;
    if(Key == LV_KEY_NEXT) mModel->Selected = LV_MIN(Old + mRows, mModel->Count - 1);
    if(Key == LV_KEY_PREV) mModel->Selected = Old >= (size_t)mRows ? Old - mRows : 0;
    if(Key == LV_KEY_HOME) mModel->Selected = 0;
    if(Key == LV_KEY_END) mModel->Selected = mModel->Count - 1;
    mDirty |= Old != mModel->Selected;
  }
}
FLANGE_ACTION FlangeViewPoll(void)
{
  if(mDirty) { Render(); mDirty = false; }
  FLANGE_ACTION Action = mAction;
  mAction = FlangeNone;
  return Action;
}
