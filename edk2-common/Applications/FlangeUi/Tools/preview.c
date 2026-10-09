/* Headless rendering and keyboard regression checks of the actual firmware view.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FlangeView.h"
#include "FlangeText.h"
#include "FlangePanel.h"
#include "FlangeValue.h"
static uint8_t *Pixels;
static int Width, Height;
static void Flush(lv_display_t *Display, const lv_area_t *Area, uint8_t *Data)
{
  for(int y=Area->y1; y<=Area->y2; y++) {
    size_t n=(size_t)(Area->x2-Area->x1+1)*4;
    memcpy(Pixels+((size_t)y*Width+Area->x1)*4, Data, n);
    Data+=n;
  }
  lv_display_flush_ready(Display);
}
static lv_obj_t *FindLabel(lv_obj_t *Parent, const char *Value)
{
  if(lv_obj_check_type(Parent, &lv_label_class) && !strcmp(lv_label_get_text(Parent), Value)) return Parent;
  for(uint32_t i=0;i<lv_obj_get_child_count(Parent);i++) {
    lv_obj_t *Found=FindLabel(lv_obj_get_child(Parent,(int32_t)i),Value);
    if(Found) return Found;
  }
  return NULL;
}
static void CheckLayout(lv_obj_t *Parent)
{
  for(uint32_t i=0;i<lv_obj_get_child_count(Parent);i++) {
    lv_obj_t *Child=lv_obj_get_child(Parent,(int32_t)i);
    if(lv_obj_check_type(Child, &lv_label_class)) {
      assert(lv_obj_get_x(Child)>=0 && lv_obj_get_y(Child)>=0);
      assert(lv_obj_get_x(Child)+lv_obj_get_width(Child)<=lv_obj_get_width(Parent));
      if(!lv_obj_has_flag(Parent,LV_OBJ_FLAG_SCROLLABLE)) assert(lv_obj_get_y(Child)+lv_obj_get_height(Child)<=lv_obj_get_height(Parent));
    }
    CheckLayout(Child);
  }
}
static void CheckUtf16(void)
{
  const uint16_t Input[]={0xfff1,0x4e2d,0x6587,0xfff0,'A',0xd83d,0xde00,0};
  char Output[32];
  FlangeUtf16ToUtf8(Input,Output,sizeof(Output));
  assert(!strcmp(Output,"中文A\xf0\x9f\x98\x80"));
  FlangeUtf16ToUtf8(Input,Output,4); assert(!strcmp(Output,"中"));
  FlangeUtf16ToUtf8(Input,Output,3); assert(!strcmp(Output,""));
  const uint16_t Invalid[]={0xd800,'X',0xdc00,0};
  FlangeUtf16ToUtf8(Invalid,Output,sizeof(Output));
  assert(!strcmp(Output,"\xef\xbf\xbdX\xef\xbf\xbd"));
  FlangeUtf16ToUtf8(NULL,Output,sizeof(Output)); assert(!Output[0]);
  Output[0]='A'; FlangeUtf16ToUtf8(Input,Output,0); assert(Output[0]=='A');
}
static bool Parse(const char *Text,size_t Base,bool Signed,size_t Width,uint64_t *Out)
{
  uint16_t Buffer[128]; size_t i=0; for(;Text[i] && i<127;i++) Buffer[i]=(uint8_t)Text[i]; Buffer[i]=0;
  return FlangeParseNumber(Buffer,Base,Signed,Width,Out);
}
static void ValueChecks(void)
{
  uint64_t V;
  assert(Parse("255",10,false,1,&V) && V==255);
  assert(!Parse("256",10,false,1,&V));
  assert(Parse("-128",10,true,1,&V) && (int64_t)V==-128);
  assert(!Parse("-129",10,true,1,&V)); assert(!Parse("128",10,true,1,&V));
  assert(Parse("0xFFFF",16,false,2,&V) && V==65535);
  assert(!Parse("0x10000",16,false,2,&V)); assert(!Parse("-1",10,false,2,&V));
  assert(Parse("4294967295",10,false,4,&V) && V==UINT32_MAX);
  assert(!Parse("4294967296",10,false,4,&V));
  assert(Parse("18446744073709551615",10,false,8,&V) && V==UINT64_MAX);
  assert(!Parse("18446744073709551616",10,false,8,&V));
  assert(Parse("-9223372036854775808",10,true,8,&V) && V==((uint64_t)1<<63));
  assert(Parse("9223372036854775807",10,true,8,&V) && V==INT64_MAX);
  assert(!Parse("9223372036854775808",10,true,8,&V));
  assert(!Parse("1junk",10,false,8,&V)); assert(!Parse("",10,false,1,&V));
  assert(!Parse("-",10,true,1,&V)); assert(!Parse("0x",16,false,1,&V));
  assert(!Parse("1",10,false,3,&V));
  assert(FlangeValidDate(2000,2,29)); assert(!FlangeValidDate(1900,2,29));
  assert(FlangeValidDate(2024,2,29)); assert(!FlangeValidDate(2025,2,29));
  assert(!FlangeValidDate(2026,0,1)); assert(!FlangeValidDate(2026,13,1));
  assert(!FlangeValidDate(2026,4,31)); assert(!FlangeValidDate(2026,1,0));
}
static void PanelChecks(lv_display_t *Display)
{
  FLANGE_ROW Rows[19]={0};
  for(size_t i=0;i<19;i++) {
    snprintf(Rows[i].Label,sizeof(Rows[i].Label),"%s / %zu",i%2?"Boot option / 启动项":"Firmware setting / 固件设置",i+1);
    snprintf(Rows[i].Value,sizeof(Rows[i].Value),"%s",i%2?"Enabled / 开启":"Read only / 只读");
    snprintf(Rows[i].Help,sizeof(Rows[i].Help),"Details for the selected firmware setting.\n所选固件设置的完整说明。\nThe browser retains validation and configuration ownership.");
    Rows[i].Enabled=i%2; Rows[i].Changed=i==1;
  }
  for(int t=0;t<4;t++) {
    FlangeTheme=(FLANGE_THEME)t;
    FLANGE_PANEL P={.Rows=Rows,.Count=19,.Chinese=true};
    strcpy(P.Title,"设备配置 / DEVICE CONFIGURATION");
    FlangePanelCreate(&P); lv_refr_now(Display); CheckLayout(lv_screen_active());
    FlangePanelKey(&P,13); assert(P.Action==PanelNone);
    FlangePanelKey(&P,FLANGE_SCAN_UP); assert(P.Selected==18);
    FlangePanelKey(&P,FLANGE_SCAN_DOWN); assert(P.Selected==0);
    FlangePanelKey(&P,FLANGE_SCAN_PGDN); assert(P.Selected>0);
    FlangePanelKey(&P,FLANGE_SCAN_HOME); assert(P.Selected==0);
    FlangePanelKey(&P,FLANGE_SCAN_DOWN); FlangePanelPoll(&P);
    FlangePanelKey(&P,13); assert(P.Action==PanelSelect);
    FlangePanelCreate(&P); FlangePanelKey(&P,FLANGE_SCAN_ESC); assert(P.Action==PanelBack);
    P.Reorder=true; FlangePanelCreate(&P); FlangePanelKey(&P,'+'); assert(P.Action==PanelKey && P.Key=='+');
    P.Rows=NULL; P.Count=0; P.Reorder=false;
    uint16_t Edit[5]={0}; P.Input=Edit; P.Capacity=5; P.Password=true;
    strcpy(P.Description,"输入内容 / Enter a value");
    FlangePanelCreate(&P);
    FlangePanelKey(&P,'A'); FlangePanelKey(&P,0x4e2d); FlangePanelKey(&P,0xd83d); FlangePanelKey(&P,0xde00);
    FlangePanelKey(&P,'X'); assert(Edit[3]==0xde00 && Edit[4]==0);
    FlangePanelKey(&P,8); assert(Edit[2]==0);
    FlangePanelPoll(&P); lv_refr_now(Display); CheckLayout(lv_screen_active());
    assert(FindLabel(lv_screen_active(),"**"));
    FlangePanelKey(&P,13); assert(P.Action==PanelSelect);
    P.Input=NULL; P.Capacity=0; P.Raw=true;
    FlangePanelCreate(&P); FlangePanelKey(&P,'Y'); assert(P.Action==PanelKey && P.Key=='Y');
    lv_refr_now(Display); CheckLayout(lv_screen_active());
    P.Raw=false; P.ReadOnly=true;
    FlangePanelCreate(&P); assert(FlangePanelPoll(&P));
  }
}
int main(int argc, char **argv)
{
  CheckUtf16();
  ValueChecks();
  Width=argc>1?atoi(argv[1]):1920; Height=argc>2?atoi(argv[2]):1080;
  bool Zh=argc>3 && !strcmp(argv[3],"zh");
  assert(Width>=800 && Height>=600);
  Pixels=calloc((size_t)Width*Height,4);
  void *Buffer=malloc((size_t)Width*64*4);
  lv_init();
  lv_display_t *Display=lv_display_create(Width,Height);
  lv_display_set_color_format(Display,LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_buffers(Display,Buffer,NULL,(uint32_t)Width*64*4,LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(Display,Flush);
  FLANGE_ENTRY Entries[19]={0};
  const char *Names[]={"Ubuntu","UEFI USB Device / USB 启动设备","Internal UEFI Shell","UEFI Network / 网络启动"};
  for(size_t i=0;i<19;i++) {
    snprintf(Entries[i].Title,sizeof(Entries[i].Title),"%s%s",Names[i%4],i>3?" / long boot option description for overflow testing":"");
    snprintf(Entries[i].Path,sizeof(Entries[i].Path),"PciRoot(0x0)/Pci(0x0,0x0)/USB(0x1,0x0)/HD(1,GPT,...)/\\EFI\\BOOT\\BOOTAA64.EFI");
    Entries[i].Number=(uint16_t)i;
  }
  FLANGE_MODEL Model={.Entries=Entries,.Count=19,.Chinese=Zh,.SecureBoot=0};
  PanelChecks(Display);
  FlangeTheme=argc>6?(FLANGE_THEME)atoi(argv[6]):FlangeField;
  FlangeViewCreate(&Model);
  lv_obj_t *Next=FindLabel(lv_screen_active(),">"); assert(Next);
  lv_obj_send_event(lv_obj_get_parent(Next),LV_EVENT_CLICKED,NULL);
  assert(Model.Selected>0); FlangeViewPoll();
  FlangeViewKey(LV_KEY_HOME); FlangeViewPoll();
  FlangeViewKey(LV_KEY_UP); assert(Model.Selected==18);
  FlangeViewKey(LV_KEY_DOWN); assert(Model.Selected==0);
  FlangeViewKey(LV_KEY_NEXT); assert(Model.Selected>0 && Model.Selected<19);
  FlangeViewKey(LV_KEY_END); assert(Model.Selected==18);
  FlangeViewKey('\r'); assert(FlangeViewPoll()==FlangeBoot && Model.Selected==18);
  FlangeViewKey(LV_KEY_ENTER); assert(FlangeViewPoll()==FlangeBoot);
  FlangeViewKey('t'); assert(FlangeViewPoll()==FlangeThemes);
  FlangeViewKey('s'); assert(FlangeViewPoll()==FlangeSetup);
  FlangeViewKey('r'); assert(FlangeViewPoll()==FlangeRefresh);
  FlangeViewKey('l'); assert(FlangeViewPoll()==FlangeLanguage);
  FlangeViewKey(LV_KEY_ESC); assert(FlangeViewPoll()==FlangeExit);
  FlangeViewKey(LV_KEY_HOME); assert(Model.Selected==0);
  FlangeViewPoll();
  Model.Count=argc>5?(size_t)atoi(argv[5]):4;
  assert(Model.Count<=19);
  FlangeViewCreate(&Model);
  lv_refr_now(Display);
  CheckLayout(lv_screen_active());
  if(argc>7) {
    FLANGE_ROW Rows[5]={0};
    const char *Names[]={"Field","Terminal","Paper","Wartime","HII"};
    const char *Values[]={"工程现场 / 信号黄","调度终端 / 青色","研究档案 / 橄榄黄","战时指令 / 绯红","原版 UEFI 界面"};
    for(int i=0;i<5;i++) { strcpy(Rows[i].Label,Names[i]); strcpy(Rows[i].Value,Values[i]); Rows[i].Enabled=true; }
    FLANGE_PANEL P={.Rows=Rows,.Count=5,.Selected=FlangeTheme,.Chinese=true};
    strcpy(P.Title,"菜单主题"); strcpy(P.Description,"主题应用到所有菜单，并在下次启动时使用。");
    FlangePanelCreate(&P); lv_refr_now(Display); CheckLayout(lv_screen_active());
  }
  FILE *Out=fopen(argc>4?argv[4]:"preview.ppm","wb"); assert(Out);
  fprintf(Out,"P6\n%d %d\n255\n",Width,Height);
  for(size_t i=0;i<(size_t)Width*Height;i++) {
    fputc(Pixels[i*4+2],Out); fputc(Pixels[i*4+1],Out); fputc(Pixels[i*4],Out);
  }
  fclose(Out);
  FlangeViewCreate(&Model);
  /* Empty list never issues a boot action; all navigation remains valid. */
  Model.Count=0; Model.Selected=0;
  FlangeViewKey(LV_KEY_DOWN); FlangeViewKey(LV_KEY_UP);
  FlangeViewKey(LV_KEY_ENTER); assert(FlangeViewPoll()==FlangeNone);
  FlangeViewKey('\r'); assert(FlangeViewPoll()==FlangeNone);
  assert(Model.Selected==0);
  /* Repeated selection and redraw should not accumulate objects. */
  Model.Count=19;
  for(int i=0;i<100;i++) { FlangeViewKey(LV_KEY_DOWN); FlangeViewPoll(); lv_refr_now(Display); }
  lv_deinit(); free(Buffer); free(Pixels);
  puts("render, layout, keyboard, mouse paging and UTF-16 checks passed");
  return 0;
}
