/* Common form, option picker, text editor, and dialog surface.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FlangePanel.h"
#include "FlangeText.h"
static FLANGE_PANEL *M;
static int32_t RowsPerPage;
static bool Dirty;
static lv_obj_t *Help;
static const FLANGE_PALETTE *P;
static lv_obj_t *Box(lv_obj_t *Parent,int x,int y,int w,int h,uint32_t Color)
{
  lv_obj_t *O=lv_obj_create(Parent); lv_obj_remove_style_all(O);
  lv_obj_set_pos(O,x,y); lv_obj_set_size(O,w,h);
  lv_obj_set_style_bg_color(O,lv_color_hex(Color),0); lv_obj_set_style_bg_opa(O,255,0);
  lv_obj_remove_flag(O,LV_OBJ_FLAG_SCROLLABLE); return O;
}
static lv_obj_t *Text(lv_obj_t *Parent,int x,int y,int w,const char *S,uint32_t Color,const lv_font_t *Font)
{
  lv_obj_t *O=lv_label_create(Parent); lv_obj_set_pos(O,x,y); lv_obj_set_width(O,w);
  lv_label_set_text(O,S); lv_obj_set_style_text_color(O,lv_color_hex(Color),0);
  lv_obj_set_style_text_font(O,Font,0); lv_obj_remove_flag(O,LV_OBJ_FLAG_CLICKABLE); return O;
}
static void Click(lv_event_t *E)
{
  uint32_t K=(uint32_t)(uintptr_t)lv_event_get_user_data(E);
  if(K>=0x20000) { M->Selected=K-0x20000; Dirty=true; }
  else FlangePanelKey(M,K);
}
static void Button(lv_obj_t *S,int x,int y,int w,const char *Label,uint32_t Key,bool Primary)
{
  lv_obj_t *B=Box(S,x,y,w,48,Primary?P->Brand:P->Surface);
  lv_obj_set_style_border_width(B,1,0); lv_obj_set_style_border_color(B,lv_color_hex(P->Rule),0);
  lv_obj_add_flag(B,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(B,Click,LV_EVENT_CLICKED,(void*)(uintptr_t)Key);
  lv_obj_t *L=Text(B,8,13,w-16,Label,Primary?P->OnBrand:P->Ink,&FlangeSmall18);
  lv_obj_set_style_text_align(L,LV_TEXT_ALIGN_CENTER,0);
}
static void Ground(lv_event_t *E)
{
  lv_layer_t *L=lv_event_get_layer(E); lv_draw_rect_dsc_t D; lv_draw_rect_dsc_init(&D);
  int W=lv_display_get_horizontal_resolution(NULL), H=lv_display_get_vertical_resolution(NULL);
  D.bg_color=lv_color_hex(P->Rule);
  for(int I=28;I>=0;I-=4) {
    D.bg_opa=5; lv_area_t A={W*3/5-I,H/6-I,W*3/5+16+I,H*5/6+I}; lv_draw_rect(L,&D,&A);
    A.x2=W*4/5+I; A.y2=H/6+16+I; lv_draw_rect(L,&D,&A);
  }
  if(M->Count && !M->Input && RowsPerPage>0) {
    int X=W/20+(W-W/10)*57/100, Y=156+(int)(M->Selected%(size_t)RowsPerPage)*64+32;
    lv_draw_line_dsc_t Link; lv_draw_line_dsc_init(&Link); Link.color=lv_color_hex(P->Rule); Link.width=1;
    Link.p1.x=X; Link.p1.y=Y;
    for(int I=1;I<=16;I++) { int T=I*16, Smooth=T*T*(768-2*T)/65536;
      Link.p2.x=X+24*T/256; Link.p2.y=Y+(208-Y)*Smooth/256; lv_draw_line(L,&Link); Link.p1=Link.p2;
    }
  }
  D.bg_opa=25;
  for(int y=4;y<H;y+=24) for(int x=4;x<W;x+=24) {
    lv_area_t A={x,y,x,y}; lv_draw_rect(L,&D,&A);
  }
  if(FlangeTheme==FlangeTerminal) {
    D.bg_opa=6;
    for(int y=0;y<H;y+=4) { lv_area_t A={0,y,W-1,y}; lv_draw_rect(L,&D,&A); }
  }
}
static void Render(void)
{
  P=FlangePalette(); lv_obj_t *S=lv_screen_active(); lv_obj_clean(S);
  lv_obj_set_style_bg_color(S,lv_color_hex(P->Ground),0);
  int W=lv_display_get_horizontal_resolution(NULL),H=lv_display_get_vertical_resolution(NULL), X=W/20, CW=W-X*2;
  int Top=156, Left=CW*57/100, Gap=24, Right=CW-Left-Gap, RH=64;
  RowsPerPage=(H-Top-150)/RH; if(RowsPerPage<1) RowsPerPage=1; if(RowsPerPage>9) RowsPerPage=9;
  Box(S,X,34,12,2,P->Ink); Box(S,X,34,2,24,P->Ink);
  Box(S,X+22,58,12,2,P->Ink); Box(S,X+32,36,2,24,P->Ink);
  if(FlangeTheme==FlangeWartime) { Box(S,X+44,24,CW-44,44,P->Panel); Box(S,X+44,24,4,44,P->Brand); }
  Text(S,X+52,36,CW/2,"FLANGE / CONFIGURATION",FlangeTheme==FlangeWartime?0xffffff:P->Ink,FlangeTheme==FlangeTerminal?&FlangeMono20:&FlangeBody20);
  lv_obj_t *Mode=Text(S,X+CW/2,40,CW/2,P->Name,FlangeTheme==FlangeWartime?0xffffff:P->Muted,&FlangeMono14);
  lv_obj_set_style_text_align(Mode,LV_TEXT_ALIGN_RIGHT,0); Box(S,X,76,CW,1,P->Rule);
  lv_obj_t *Title=Text(S,X,98,CW,M->Title,P->Ink,&FlangeCjk24);
  lv_obj_set_height(Title,32); lv_label_set_long_mode(Title,LV_LABEL_LONG_DOT);
  Help=NULL;
  if(M->Input || !M->Count) {
    Left=CW; Right=0;
    lv_obj_t *Body=Box(S,X,Top,CW,H-Top-134,P->Surface);
    Help=Box(Body,24,18,CW-48,H-Top-230,P->Surface);
    lv_obj_add_flag(Help,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(Help,LV_DIR_VER);
    Text(Help,0,0,CW-64,M->Description,P->Ink,&FlangeCjk24);
    if(M->Input) {
      char Buf[4096]; size_t N=0;
      while(N+1<M->Capacity && M->Input[N]) N++;
      if(M->Password) { size_t Limit=LV_MIN(N,sizeof(Buf)-1); for(size_t i=0;i<Limit;i++) Buf[i]='*'; Buf[Limit]=0; }
      else FlangeUtf16ToUtf8(M->Input,Buf,sizeof(Buf));
      lv_obj_t *Entry=Box(Body,24,H-Top-202,CW-48,52,P->Ground);
      Box(Entry,0,50,CW-48,2,P->Brand);
      lv_obj_t *Input=Text(Entry,12,10,CW-72,Buf,P->Ink,&FlangeCjk24);
      lv_obj_set_height(Input,32); lv_label_set_long_mode(Input,LV_LABEL_LONG_DOT);
    }
  } else {
    size_t Start=M->Selected/(size_t)RowsPerPage*(size_t)RowsPerPage;
    for(size_t I=Start;I<M->Count && I<Start+(size_t)RowsPerPage;I++) {
      FLANGE_ROW *R=&M->Rows[I]; bool Sel=I==M->Selected;
      uint32_t Ink=Sel && R->Enabled?P->OnBrand:R->Enabled?P->Ink:P->Muted;
      lv_obj_t *Row=Box(S,X,Top+(int)(I-Start)*RH,Left,RH-5,Sel && R->Enabled?P->Brand:P->Surface);
      if(Sel && !R->Enabled) { lv_obj_set_style_border_width(Row,1,0); lv_obj_set_style_border_color(Row,lv_color_hex(P->Rule),0); }
      lv_obj_add_flag(Row,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(Row,Click,LV_EVENT_CLICKED,(void*)(uintptr_t)(0x20000+I));
      lv_obj_t *Label=Text(Row,16,7,Left-32,R->Label,Ink,&FlangeCjk24);
      lv_obj_set_height(Label,29); lv_label_set_long_mode(Label,LV_LABEL_LONG_DOT);
      lv_obj_t *Value=Text(Row,16,32,Left-32,R->Value,Ink,&FlangeCjk24);
      lv_obj_set_height(Value,26); lv_label_set_long_mode(Value,LV_LABEL_LONG_DOT);
      if(R->Changed) Box(Row,0,0,4,RH-5,Sel?P->OnBrand:P->Brand);
    }
    int DH=RowsPerPage*RH-5;
    lv_obj_t *Details=Box(S,X+Left+Gap,Top,Right,DH,P->Surface);
    Text(Details,20,18,Right-40,M->Chinese?"说明":"DETAIL",P->Muted,&FlangeSmall18);
    Box(Details,20,50,Right-40,1,P->Rule);
    Help=Box(Details,20,66,Right-40,DH-80,P->Surface);
    lv_obj_add_flag(Help,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(Help,LV_DIR_VER);
    FLANGE_ROW *R=&M->Rows[M->Selected];
    /* Full label and value remain readable even when the list abbreviates them. */
    char Info[3072]; lv_snprintf(Info,sizeof(Info),"%s\n%s\n\n%s",R->Label,R->Value,R->Help[0]?R->Help:M->Description);
    Text(Help,0,0,Right-56,Info,P->Ink,&FlangeCjk24);
    char Page[80]; lv_snprintf(Page,sizeof(Page),"%u / %u",(unsigned)(Start/RowsPerPage+1),(unsigned)((M->Count+RowsPerPage-1)/RowsPerPage));
    Text(S,X,Top+RowsPerPage*RH+10,100,Page,P->Muted,&FlangeMono14);
    if(M->Count>(size_t)RowsPerPage) {
    Button(S,X+Left-112,Top+RowsPerPage*RH+1,50,"<",FLANGE_SCAN_PGUP,false);
    Button(S,X+Left-54,Top+RowsPerPage*RH+1,50,">",FLANGE_SCAN_PGDN,false);
    }
  }
  int BW=(CW-24)/3;
  Button(S,X,H-90,BW,M->Chinese?"Esc / 返回":"Esc / Back",FLANGE_SCAN_ESC,false);
  Button(S,X+BW+12,H-90,BW,M->Reorder?"- / +":M->Chinese?"左右键 / 滚动说明":"Left / Right: detail",M->Reorder?'+':FLANGE_SCAN_RIGHT,false);
  bool Enabled=!M->Count || M->Rows[M->Selected].Enabled;
  Button(S,X+2*(BW+12),H-90,BW,Enabled?(M->Chinese?"Enter / 确认":"Enter / Select"):(M->Chinese?"只读":"Read only"),13,Enabled);
  lv_obj_t *Foot=Text(S,X,H-30,CW,M->Footer[0]?M->Footer:M->Chinese?"上下键 选择    PageUp / PageDown 翻页":"UP / DOWN  SELECT    PGUP / PGDN  PAGE",P->Muted,&FlangeSmall18);
  lv_obj_set_height(Foot,23); lv_label_set_long_mode(Foot,LV_LABEL_LONG_DOT);
}
void FlangePanelCreate(void *Context)
{
  M=Context; M->Action=PanelNone; Dirty=false;
  if(M->Selected>=M->Count) M->Selected=0;
  lv_obj_t *S=lv_screen_active(); lv_obj_remove_style_all(S);
  while(lv_obj_get_event_count(S)) lv_obj_remove_event(S,0);
  lv_obj_set_style_bg_opa(S,255,0); lv_obj_remove_flag(S,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(S,Ground,LV_EVENT_DRAW_MAIN,NULL); Render();
}
void FlangePanelKey(void *Context,uint32_t Key)
{
  M=Context; M->Key=Key;
  if(M->Raw) { M->Action=PanelKey; return; }
  if(Key==FLANGE_SCAN_ESC) { M->Action=PanelBack; return; }
  if(Key==13) { if(!M->Count || M->Rows[M->Selected].Enabled) M->Action=PanelSelect; return; }
  if(M->Input) {
    size_t N=0; while(N+1<M->Capacity && M->Input[N]) N++;
    if(Key==8 && N) { M->Input[--N]=0; if(N && M->Input[N-1]>=0xd800 && M->Input[N-1]<=0xdbff) M->Input[N-1]=0; Dirty=true; }
    else if(Key>=32 && Key<0x10000 && N+1<M->Capacity) { M->Input[N]=(uint16_t)Key; M->Input[N+1]=0; Dirty=true; }
    return;
  }
  if(Key==FLANGE_SCAN_RIGHT || Key==FLANGE_SCAN_LEFT) {
    if(Help) lv_obj_scroll_by(Help,0,Key==FLANGE_SCAN_RIGHT?-96:96,LV_ANIM_OFF);
    return;
  }
  if(M->Reorder && (Key=='+' || Key=='-')) { M->Action=PanelKey; return; }
  if(M->Count) {
    size_t Old=M->Selected;
    if(Key==FLANGE_SCAN_DOWN) M->Selected=(Old+1)%M->Count;
    else if(Key==FLANGE_SCAN_UP) M->Selected=Old?Old-1:M->Count-1;
    else if(Key==FLANGE_SCAN_HOME) M->Selected=0;
    else if(Key==FLANGE_SCAN_END) M->Selected=M->Count-1;
    else if(Key==FLANGE_SCAN_PGDN) M->Selected=LV_MIN(Old+RowsPerPage,M->Count-1);
    else if(Key==FLANGE_SCAN_PGUP) M->Selected=Old>=(size_t)RowsPerPage?Old-RowsPerPage:0;
    else M->Action=PanelKey;
    Dirty|=Old!=M->Selected;
  } else M->Action=PanelKey;
}
bool FlangePanelPoll(void *Context) { M=Context; if(Dirty) { Render(); Dirty=false; } return M->ReadOnly || M->Action!=PanelNone; }
