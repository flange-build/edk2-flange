/** @file LVGL presentation of EDK2 browser data. The browser owns configuration,
  expressions, navigation, validation callbacks, and save/discard semantics.
  The adapters live only while this application is loaded and are restored before
  it returns. Thus fonts/LVGL are stored once, including on small firmware volumes.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>
#include <Library/DebugLib.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/DisplayProtocol.h>
#include <Protocol/FormBrowserEx2.h>
#include <Protocol/HiiPopup.h>
#include <Protocol/FlangeUi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/HiiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "FlangeForms.h"
#include "FlangeText.h"
#include "FlangeValue.h"
STATIC EFI_GUID mServiceGuid=FLANGE_UI_PROTOCOL_GUID;
STATIC EFI_GUID mThemeGuid={0x36bd4d32,0x7f5a,0x43fa,{0x9d,0xa2,0x6f,0x07,0x5f,0x88,0xee,0x23}};
STATIC EDKII_FORM_DISPLAY_ENGINE_PROTOCOL *mEngine, mOriginal;
STATIC EFI_HII_POPUP_PROTOCOL *mPopup;
STATIC EFI_HII_POPUP_PROTOCOL mOriginalPopup;
STATIC EFI_HANDLE mServiceHandle;
STATIC BOOLEAN mInstalled;
STATIC CONST CHAR8 *Tr(CONST CHAR8 *En,CONST CHAR8 *Zh) { return FlangeIsChinese()?Zh:En; }
BOOLEAN FlangeIsChinese(VOID)
{
  CHAR8 L[64]; UINTN N=sizeof(L);
  return !EFI_ERROR(gRT->GetVariable(L"PlatformLang",&gEfiGlobalVariableGuid,NULL,&N,L)) && N>=2 && L[0]=='z' && L[1]=='h';
}
VOID FlangeThemeLoad(VOID)
{
  UINT8 V; UINTN N=sizeof(V);
  FlangeTheme=FlangeField;
  if(!EFI_ERROR(gRT->GetVariable(L"FlangeMenuTheme",&mThemeGuid,NULL,&N,&V)) && N==sizeof(V) && V<FlangeThemeCount) FlangeTheme=(FLANGE_THEME)V;
}
STATIC VOID InitPanel(FLANGE_PANEL *P, CONST CHAR8 *Title)
{
  ZeroMem(P,sizeof(*P)); P->Chinese=FlangeIsChinese();
  AsciiStrCpyS(P->Title,sizeof(P->Title),Title);
}
STATIC VOID HiiText(EFI_HII_HANDLE H,EFI_STRING_ID Id,CHAR8 *Out,UINTN Size)
{
  CHAR16 *S=Id?HiiGetString(H,Id,NULL):NULL;
  FlangeUtf16ToUtf8(S,Out,Size); if(S) FreePool(S);
}
STATIC EFI_STATUS Message(CONST CHAR8 *Title,CONST CHAR8 *Text)
{
  FLANGE_PANEL P; InitPanel(&P,Title); AsciiStrCpyS(P.Description,sizeof(P.Description),Text);
  return FlangeRunPanel(&P,NULL);
}
STATIC EFI_STATUS mChoiceStatus;
STATIC INTN Choose(CONST CHAR8 *Title,CONST CHAR8 *Description,FLANGE_ROW *Rows,UINTN Count,UINTN Selected)
{
  FLANGE_PANEL P; InitPanel(&P,Title); P.Rows=Rows; P.Count=Count; P.Selected=Selected;
  AsciiStrCpyS(P.Description,sizeof(P.Description),Description);
  EFI_STATUS S=FlangeRunPanel(&P,NULL); mChoiceStatus=S;
  return !EFI_ERROR(S) && P.Action==PanelSelect?(INTN)P.Selected:-1;
}
EFI_STATUS EFIAPI FlangeChooseTheme(VOID)
{
  STATIC CONST CHAR8 *Names[]={"Field","Terminal","Paper","Wartime","HII"};
  STATIC CONST CHAR8 *En[]={"Engineering / light grey / signal yellow","Dispatch / navy / cyan","Research / warm paper / olive yellow","Operations / light grey / crimson","Original UEFI text interface"};
  STATIC CONST CHAR8 *Zh[]={"工程现场 / 浅灰 / 信号黄","调度终端 / 深蓝黑 / 青色","研究档案 / 暖纸色 / 橄榄黄","战时指令 / 浅灰 / 绯红","原版 UEFI 文本界面"};
  FLANGE_ROW *Rows=AllocateZeroPool(sizeof(*Rows)*FlangeThemeCount);
  if(!Rows) return EFI_OUT_OF_RESOURCES;
  for(UINTN I=0;I<FlangeThemeCount;I++) {
    AsciiStrCpyS(Rows[I].Label,sizeof(Rows[I].Label),Names[I]);
    AsciiStrCpyS(Rows[I].Help,sizeof(Rows[I].Help),FlangeIsChinese()?Zh[I]:En[I]);
    AsciiStrCpyS(Rows[I].Value,sizeof(Rows[I].Value),I==(UINTN)FlangeTheme?Tr("Active","当前主题"):Tr("Apply to all menus","应用到所有菜单"));
    Rows[I].Enabled=TRUE;
  }
  INTN N=Choose(Tr("Menu theme","菜单主题"),Tr("Saved for the next boot.","主题会保存并在下次启动时使用。"),Rows,FlangeThemeCount,FlangeTheme);
  FreePool(Rows); if(N<0) return EFI_ABORTED;
  UINT8 V=(UINT8)N;
  EFI_STATUS S=gRT->SetVariable(L"FlangeMenuTheme",&mThemeGuid,EFI_VARIABLE_NON_VOLATILE|EFI_VARIABLE_BOOTSERVICE_ACCESS,sizeof(V),&V);
  if(EFI_ERROR(S)) { Message(Tr("Theme not saved","主题保存失败"),Tr("Firmware variable storage could not be updated. The previous theme remains active.","无法写入固件变量，继续使用原主题。")); return S; }
  FlangeTheme=(FLANGE_THEME)V; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI LegacyPopup(EFI_INPUT_KEY *Key,UINTN Count,CHAR16 **Lines)
{
  if(FlangeTheme==FlangeHii) return EFI_UNSUPPORTED;
  FLANGE_PANEL P; InitPanel(&P,Tr("Firmware notice","固件提示")); P.Raw=Key!=NULL;
  CHAR8 Line[1024];
  for(UINTN I=0;I<Count;I++) {
    FlangeUtf16ToUtf8(Lines[I],Line,sizeof(Line));
    if(AsciiStrLen(P.Description)+AsciiStrLen(Line)+2>=sizeof(P.Description)) break;
    AsciiStrCatS(P.Description,sizeof(P.Description),Line); AsciiStrCatS(P.Description,sizeof(P.Description),"\n");
  }
  /* A NULL Key is a nonblocking status message: render one frame, then return. */
  if(!Key) P.ReadOnly=TRUE;
  EFI_STATUS S=FlangeRunPanel(&P,NULL);
  if(!EFI_ERROR(S) && Key) { Key->ScanCode=P.Key>=0x10000?(UINT16)(P.Key-0x10000):0; Key->UnicodeChar=P.Key<0x10000?(CHAR16)P.Key:0; }
  return S;
}
STATIC FLANGE_UI_PROTOCOL mService={FLANGE_UI_PROTOCOL_REVISION,FlangeChooseTheme,LegacyPopup};
STATIC EFI_STATUS EFIAPI Popup(EFI_HII_POPUP_PROTOCOL *This,EFI_HII_POPUP_STYLE Style,EFI_HII_POPUP_TYPE Type,EFI_HII_HANDLE H,EFI_STRING_ID Id,EFI_HII_POPUP_SELECTION *Selection)
{
  if(FlangeTheme==FlangeHii) return mOriginalPopup.CreatePopup(This,Style,Type,H,Id,Selection);
  if(Type>EfiHiiPopupTypeYesNoCancel || Style>EfiHiiPopupStyleError || !H || !Id) return EFI_INVALID_PARAMETER;
  CHAR8 Text[2048]; HiiText(H,Id,Text,sizeof(Text));
  FLANGE_ROW Rows[3]; ZeroMem(Rows,sizeof(Rows)); UINTN N=0; EFI_HII_POPUP_SELECTION Values[3];
  if(Type==EfiHiiPopupTypeOk || Type==EfiHiiPopupTypeOkCancel) {
    AsciiStrCpyS(Rows[N].Label,sizeof(Rows[N].Label),Tr("OK","确认")); Values[N++]=EfiHiiPopupSelectionOk;
  } else {
    AsciiStrCpyS(Rows[N].Label,sizeof(Rows[N].Label),Tr("Yes","是")); Values[N++]=EfiHiiPopupSelectionYes;
    AsciiStrCpyS(Rows[N].Label,sizeof(Rows[N].Label),Tr("No","否")); Values[N++]=EfiHiiPopupSelectionNo;
  }
  if(Type==EfiHiiPopupTypeOkCancel || Type==EfiHiiPopupTypeYesNoCancel) {
    AsciiStrCpyS(Rows[N].Label,sizeof(Rows[N].Label),Tr("Cancel","取消")); Values[N++]=EfiHiiPopupSelectionCancel;
  }
  for(UINTN I=0;I<N;I++) Rows[I].Enabled=TRUE;
  INTN Pick;
  do { Pick=Choose(Style==EfiHiiPopupStyleError?Tr("Error","错误"):Style==EfiHiiPopupStyleWarning?Tr("Warning","警告"):Tr("Confirm","确认"),Text,Rows,N,N-1); }
  while(!EFI_ERROR(mChoiceStatus) && Pick<0 && Type!=EfiHiiPopupTypeOkCancel && Type!=EfiHiiPopupTypeYesNoCancel);
  if(EFI_ERROR(mChoiceStatus)) return mOriginalPopup.CreatePopup(This,Style,Type,H,Id,Selection);
  if(Selection) *Selection=Pick<0?EfiHiiPopupSelectionCancel:Values[Pick];
  return EFI_SUCCESS;
}
STATIC UINT64 ValueNumber(CONST EFI_HII_VALUE *V)
{
  switch(V->Type) {
  case EFI_IFR_TYPE_NUM_SIZE_8: return V->Value.u8;
  case EFI_IFR_TYPE_NUM_SIZE_16: return V->Value.u16;
  case EFI_IFR_TYPE_NUM_SIZE_32: return V->Value.u32;
  case EFI_IFR_TYPE_NUM_SIZE_64: return V->Value.u64;
  case EFI_IFR_TYPE_BOOLEAN: return V->Value.b;
  default: return 0;
  }
}
STATIC UINTN ValueWidth(UINT8 T) { return T<=EFI_IFR_TYPE_NUM_SIZE_64 ? (UINTN)1<<T : T==EFI_IFR_TYPE_BOOLEAN?1:0; }
STATIC BOOLEAN IsQuestion(UINT8 Op)
{
  return Op==EFI_IFR_ONE_OF_OP || Op==EFI_IFR_CHECKBOX_OP || Op==EFI_IFR_NUMERIC_OP || Op==EFI_IFR_STRING_OP || Op==EFI_IFR_PASSWORD_OP || Op==EFI_IFR_ORDERED_LIST_OP || Op==EFI_IFR_DATE_OP || Op==EFI_IFR_TIME_OP || Op==EFI_IFR_REF_OP || Op==EFI_IFR_ACTION_OP;
}
STATIC BOOLEAN IsStatement(UINT8 Op) { return IsQuestion(Op) || Op==EFI_IFR_TEXT_OP || Op==EFI_IFR_SUBTITLE_OP || Op==EFI_IFR_RESET_BUTTON_OP; }
STATIC EFI_IFR_STATEMENT_HEADER *Header(FORM_DISPLAY_ENGINE_STATEMENT *Q) { return (EFI_IFR_STATEMENT_HEADER *)(Q->OpCode+1); }
STATIC VOID ValueText(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,CHAR8 *Out,UINTN Size)
{
  EFI_HII_VALUE *V=&Q->CurrentValue; Out[0]=0;
  switch(Q->OpCode->OpCode) {
  case EFI_IFR_ONE_OF_OP: {
    LIST_ENTRY *L;
    for(L=GetFirstNode(&Q->OptionListHead);!IsNull(&Q->OptionListHead,L);L=GetNextNode(&Q->OptionListHead,L)) {
      EFI_IFR_ONE_OF_OPTION *O=(DISPLAY_QUESTION_OPTION_FROM_LINK(L))->OptionOpCode;
      EFI_HII_VALUE Candidate; ZeroMem(&Candidate,sizeof(Candidate)); Candidate.Type=O->Type; CopyMem(&Candidate.Value,&O->Value,ValueWidth(O->Type));
      if(ValueNumber(V)==ValueNumber(&Candidate)) { HiiText(F->HiiHandle,O->Option,Out,Size); return; }
    }
    AsciiSPrint(Out,Size,"%Lu",ValueNumber(V)); break;
  }
  case EFI_IFR_CHECKBOX_OP: AsciiStrCpyS(Out,Size,V->Value.b?Tr("[x] Enabled","[x] 开启"):Tr("[ ] Disabled","[ ] 关闭")); break;
  case EFI_IFR_NUMERIC_OP: {
    EFI_IFR_NUMERIC *N=(EFI_IFR_NUMERIC *)Q->OpCode; UINT8 Format=N->Flags&EFI_IFR_DISPLAY;
    UINT64 U=ValueNumber(V); INT64 Signed=(INT64)U; UINTN Bits=ValueWidth(V->Type)*8;
    if(Bits<64 && (U&LShiftU64(1,Bits-1))) Signed=(INT64)(U|LShiftU64(MAX_UINT64,Bits));
    AsciiSPrint(Out,Size,Format==EFI_IFR_DISPLAY_UINT_HEX?"0x%Lx":Format==EFI_IFR_DISPLAY_INT_DEC?"%Ld":"%Lu",Format==EFI_IFR_DISPLAY_INT_DEC?(UINT64)Signed:U); break;
  }
  case EFI_IFR_STRING_OP: FlangeUtf16ToUtf8((CHAR16 *)V->Buffer,Out,Size); break;
  case EFI_IFR_PASSWORD_OP: AsciiStrCpyS(Out,Size,"********"); break;
  case EFI_IFR_TEXT_OP: HiiText(F->HiiHandle,((EFI_IFR_TEXT *)Q->OpCode)->TextTwo,Out,Size); break;
  case EFI_IFR_DATE_OP: AsciiSPrint(Out,Size,"%04u-%02u-%02u",V->Value.date.Year,V->Value.date.Month,V->Value.date.Day); break;
  case EFI_IFR_TIME_OP: AsciiSPrint(Out,Size,"%02u:%02u:%02u",V->Value.time.Hour,V->Value.time.Minute,V->Value.time.Second); break;
  case EFI_IFR_REF_OP: AsciiStrCpyS(Out,Size,Tr("Open >","打开 >")); break;
  case EFI_IFR_ORDERED_LIST_OP: AsciiStrCpyS(Out,Size,Tr("Edit order >","调整顺序 >")); break;
  default: break;
  }
}
STATIC BOOLEAN Visible(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q)
{
  if(!IsStatement(Q->OpCode->OpCode)) return FALSE;
  if(Q->OpCode->OpCode==EFI_IFR_SUBTITLE_OP || Q->OpCode->OpCode==EFI_IFR_TEXT_OP) {
    CHAR8 Label[256]; HiiText(F->HiiHandle,Header(Q)->Prompt,Label,sizeof(Label));
    BOOLEAN NonSpace=FALSE;
    for(UINTN I=0;Label[I];I++) if((UINT8)Label[I]>32) { NonSpace=TRUE; break; }
    if(!NonSpace) return FALSE;
  }
  return TRUE;
}
STATIC UINTN CountRows(FORM_DISPLAY_ENGINE_FORM *F,LIST_ENTRY *Head)
{
  UINTN N=0; LIST_ENTRY *L;
  for(L=GetFirstNode(Head);!IsNull(Head,L);L=GetNextNode(Head,L)) {
    FORM_DISPLAY_ENGINE_STATEMENT *Q=FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK(L);
    if(Q->Attribute&HII_DISPLAY_SUPPRESS) continue;
    if(Visible(F,Q)) N++;
    N+=CountRows(F,&Q->NestStatementList);
  }
  return N;
}
STATIC VOID FillRows(FORM_DISPLAY_ENGINE_FORM *F,LIST_ENTRY *Head,FLANGE_PANEL *P,FORM_DISPLAY_ENGINE_STATEMENT **Map,UINTN *Index,UINT32 Parent)
{
  LIST_ENTRY *L;
  for(L=GetFirstNode(Head);!IsNull(Head,L);L=GetNextNode(Head,L)) {
    FORM_DISPLAY_ENGINE_STATEMENT *Q=FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK(L);
    if(Q->Attribute&HII_DISPLAY_SUPPRESS) continue;
    UINT32 Attr=Q->Attribute|Parent;
    if(Visible(F,Q)) {
      FLANGE_ROW *R=&P->Rows[*Index]; Map[*Index]=Q;
      HiiText(F->HiiHandle,Header(Q)->Prompt,R->Label,sizeof(R->Label));
      HiiText(F->HiiHandle,Header(Q)->Help,R->Help,sizeof(R->Help));
      ValueText(F,Q,R->Value,sizeof(R->Value));
      R->Enabled=!(Attr&(HII_DISPLAY_GRAYOUT|HII_DISPLAY_LOCK|HII_DISPLAY_READONLY)) && (IsQuestion(Q->OpCode->OpCode)||Q->OpCode->OpCode==EFI_IFR_RESET_BUTTON_OP);
      R->Group=Q->OpCode->OpCode==EFI_IFR_SUBTITLE_OP; R->Changed=Q->SettingChangedFlag;
      if(Q==F->HighLightedStatement) P->Selected=*Index;
      (*Index)++;
    }
    FillRows(F,&Q->NestStatementList,P,Map,Index,Attr);
  }
}
STATIC EFI_STATUS EditText(CONST CHAR8 *Title,CONST CHAR8 *Help,CHAR16 *Buffer,UINTN Capacity,BOOLEAN Password)
{
  FLANGE_PANEL P; InitPanel(&P,Title); AsciiStrCpyS(P.Description,sizeof(P.Description),Help);
  P.Input=Buffer; P.Capacity=Capacity; P.Password=Password;
  EFI_STATUS S=FlangeRunPanel(&P,NULL);
  return EFI_ERROR(S)?S:P.Action==PanelSelect?EFI_SUCCESS:EFI_ABORTED;
}
STATIC EFI_STATUS PickOption(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,EFI_HII_VALUE *V,CONST CHAR8 *Title)
{
  LIST_ENTRY *L; UINTN N=0,Current=0;
  for(L=GetFirstNode(&Q->OptionListHead);!IsNull(&Q->OptionListHead,L);L=GetNextNode(&Q->OptionListHead,L)) N++;
  if(!N) return EFI_NOT_FOUND;
  FLANGE_ROW *R=AllocateZeroPool(N*sizeof(*R)); EFI_IFR_ONE_OF_OPTION **Map=AllocatePool(N*sizeof(*Map));
  if(!R||!Map) { if(R) FreePool(R); if(Map) FreePool(Map); return EFI_OUT_OF_RESOURCES; }
  N=0;
  for(L=GetFirstNode(&Q->OptionListHead);!IsNull(&Q->OptionListHead,L);L=GetNextNode(&Q->OptionListHead,L)) {
    EFI_IFR_ONE_OF_OPTION *O=(DISPLAY_QUESTION_OPTION_FROM_LINK(L))->OptionOpCode; Map[N]=O;
    HiiText(F->HiiHandle,O->Option,R[N].Label,sizeof(R[N].Label)); R[N].Enabled=TRUE;
    EFI_HII_VALUE C; ZeroMem(&C,sizeof(C)); C.Type=O->Type; CopyMem(&C.Value,&O->Value,ValueWidth(O->Type));
    if(ValueNumber(V)==ValueNumber(&C)) Current=N;
    N++;
  }
  INTN Pick=Choose(Title,"",R,N,Current);
  if(Pick>=0) { V->Type=Map[Pick]->Type; ZeroMem(&V->Value,sizeof(V->Value)); CopyMem(&V->Value,&Map[Pick]->Value,ValueWidth(V->Type)); }
  FreePool(R); FreePool(Map); return Pick>=0?EFI_SUCCESS:EFI_ABORTED;
}
STATIC EFI_STATUS OrderedList(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,EFI_HII_VALUE *V,CONST CHAR8 *Title)
{
  UINTN W=ValueWidth(Q->CurrentValue.Type); /* Ordered lists use Buffer type; obtain element type from options. */
  if(IsListEmpty(&Q->OptionListHead) || !Q->CurrentValue.Buffer) return EFI_NOT_FOUND;
  W=ValueWidth((DISPLAY_QUESTION_OPTION_FROM_LINK(GetFirstNode(&Q->OptionListHead)))->OptionOpCode->Type);
  if(!W || Q->CurrentValue.BufferLen%W) return EFI_COMPROMISED_DATA;
  UINTN Max=Q->CurrentValue.BufferLen/W, N=0;
  UINT8 *B=AllocateCopyPool(Q->CurrentValue.BufferLen,Q->CurrentValue.Buffer);
  FLANGE_ROW *Rows=AllocateZeroPool(Max*sizeof(*Rows));
  if(!B||!Rows) { if(B) FreePool(B); if(Rows) FreePool(Rows); return EFI_OUT_OF_RESOURCES; }
  for(UINTN I=0;I<Max;I++) {
    UINT64 U=0; CopyMem(&U,B+I*W,W); if(!U) break; N++;
    LIST_ENTRY *L;
    AsciiSPrint(Rows[I].Label,sizeof(Rows[I].Label),"%Lu",U);
    for(L=GetFirstNode(&Q->OptionListHead);!IsNull(&Q->OptionListHead,L);L=GetNextNode(&Q->OptionListHead,L)) {
      EFI_IFR_ONE_OF_OPTION *O=(DISPLAY_QUESTION_OPTION_FROM_LINK(L))->OptionOpCode; UINT64 C=0; CopyMem(&C,&O->Value,W);
      if(C==U) { HiiText(F->HiiHandle,O->Option,Rows[I].Label,sizeof(Rows[I].Label)); break; }
    }
    Rows[I].Enabled=TRUE;
  }
  FLANGE_PANEL P; InitPanel(&P,Title); P.Rows=Rows; P.Count=N; P.Reorder=TRUE;
  AsciiStrCpyS(P.Description,sizeof(P.Description),Tr("Select an item; - moves up, + moves down. Enter applies the order; Esc cancels.","选择项目，- 上移，+ 下移。Enter 应用顺序，Esc 取消。"));
  EFI_STATUS S;
  for(;;) {
    S=FlangeRunPanel(&P,NULL); if(EFI_ERROR(S)||P.Action==PanelBack) { if(!EFI_ERROR(S)) S=EFI_ABORTED; break; }
    if(P.Action==PanelSelect) { V->Buffer=B; B=NULL; break; }
    if(P.Action==PanelKey && N) {
      UINTN I=P.Selected,J=I;
      if(P.Key=='-'&&I) J--;
      if(P.Key=='+'&&I+1<N) J++;
      if(I!=J) { FLANGE_ROW Temp=Rows[I]; Rows[I]=Rows[J]; Rows[J]=Temp; UINT8 T[8]; CopyMem(T,B+I*W,W); CopyMem(B+I*W,B+J*W,W); CopyMem(B+J*W,T,W); P.Selected=J; }
    }
  }
  if(B) FreePool(B);
  FreePool(Rows); return S;
}
STATIC INT64 SignedValue(UINT64 U,UINTN W)
{
  UINTN Bits=W*8;
  if(Bits<64 && (U&LShiftU64(1,Bits-1))) U|=LShiftU64(MAX_UINT64,Bits);
  return (INT64)U;
}
STATIC EFI_STATUS Numeric(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,EFI_HII_VALUE *V,CONST CHAR8 *Title)
{
  EFI_IFR_NUMERIC *N=(EFI_IFR_NUMERIC *)Q->OpCode; UINTN W=ValueWidth(V->Type);
  if(!W) return EFI_UNSUPPORTED;
  UINT8 Format=N->Flags&EFI_IFR_DISPLAY; BOOLEAN Signed=Format==EFI_IFR_DISPLAY_INT_DEC;
  UINT64 Min=0,Max=0,Step=0; CopyMem(&Min,&N->data,W); CopyMem(&Max,(UINT8 *)&N->data+W,W); CopyMem(&Step,(UINT8 *)&N->data+2*W,W);
  CHAR8 Current[80],Help[256]; CHAR16 Buffer[40]; ValueText(F,Q,Current,sizeof(Current)); AsciiStrToUnicodeStrS(Current,Buffer,40);
  if(Signed) AsciiSPrint(Help,sizeof(Help),"%Ld ... %Ld  /  STEP %Lu",SignedValue(Min,W),SignedValue(Max,W),Step);
  else AsciiSPrint(Help,sizeof(Help),Format==EFI_IFR_DISPLAY_UINT_HEX?"0x%Lx ... 0x%Lx / STEP 0x%Lx":"%Lu ... %Lu / STEP %Lu",Min,Max,Step);
  for(;;) {
    EFI_STATUS S=EditText(Title,Help,Buffer,40,FALSE); if(EFI_ERROR(S)) return S;
    uint64_t U; BOOLEAN Valid=FlangeParseNumber(Buffer,Format==EFI_IFR_DISPLAY_UINT_HEX?16:10,Signed,W,&U);
    if(Valid) Valid=Signed?SignedValue(U,W)>=SignedValue(Min,W)&&SignedValue(U,W)<=SignedValue(Max,W):U>=Min&&U<=Max;
    /* The browser permits direct input independent of increment step. */
    if(Valid) { ZeroMem(&V->Value,sizeof(V->Value)); CopyMem(&V->Value,&U,W); return EFI_SUCCESS; }
    Message(Tr("Invalid value","数值无效"),Help);
  }
}
STATIC EFI_STATUS DateTime(FORM_DISPLAY_ENGINE_STATEMENT *Q,EFI_HII_VALUE *V,CONST CHAR8 *Title)
{
  BOOLEAN Date=Q->OpCode->OpCode==EFI_IFR_DATE_OP;
  UINT8 Flags=Date?((EFI_IFR_DATE *)Q->OpCode)->Flags:((EFI_IFR_TIME *)Q->OpCode)->Flags;
  UINTN Parts[3];
  if(Date) { Parts[0]=V->Value.date.Year; Parts[1]=V->Value.date.Month; Parts[2]=V->Value.date.Day; }
  else { Parts[0]=V->Value.time.Hour; Parts[1]=V->Value.time.Minute; Parts[2]=V->Value.time.Second; }
  for(UINTN I=0;I<3;I++) {
    if(Flags&(1<<I)) continue;
    CHAR16 B[16]; UnicodeSPrint(B,sizeof(B),L"%u",Parts[I]);
    CONST CHAR8 *Label=Date?(I==0?Tr("Year (1900–9999)","年份 (1900–9999)"):I==1?Tr("Month (1–12)","月份 (1–12)"):Tr("Day","日期")):(I==0?Tr("Hour (0–23)","时 (0–23)"):I==1?Tr("Minute (0–59)","分 (0–59)"):Tr("Second (0–59)","秒 (0–59)"));
    UINTN Max=Date?(I==0?9999:I==1?12:31):(I==0?23:59),Min=Date?(I==0?1900:1):0;
    for(;;) {
      EFI_STATUS S=EditText(Title,Label,B,16,FALSE); if(EFI_ERROR(S)) return S;
      uint64_t U; if(FlangeParseNumber(B,10,FALSE,4,&U)&&U>=Min&&U<=Max) { Parts[I]=(UINTN)U; break; }
      Message(Tr("Invalid value","数值无效"),Label);
    }
  }
  if(Date) {
    if(!FlangeValidDate((unsigned)Parts[0],(unsigned)Parts[1],(unsigned)Parts[2])) { Message(Tr("Invalid date","日期无效"),Tr("This month has fewer days.","日期超出该月份的天数。")); return EFI_ABORTED; }
    V->Value.date.Year=(UINT16)Parts[0]; V->Value.date.Month=(UINT8)Parts[1]; V->Value.date.Day=(UINT8)Parts[2];
  } else { V->Value.time.Hour=(UINT8)Parts[0]; V->Value.time.Minute=(UINT8)Parts[1]; V->Value.time.Second=(UINT8)Parts[2]; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS StringValue(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,EFI_HII_VALUE *V,CONST CHAR8 *Title)
{
  BOOLEAN Password=Q->OpCode->OpCode==EFI_IFR_PASSWORD_OP;
  UINTN Max=Password?((EFI_IFR_PASSWORD *)Q->OpCode)->MaxSize:((EFI_IFR_STRING *)Q->OpCode)->MaxSize;
  UINTN Min=Password?((EFI_IFR_PASSWORD *)Q->OpCode)->MinSize:((EFI_IFR_STRING *)Q->OpCode)->MinSize;
  UINTN Bytes=Q->CurrentValue.BufferLen;
  if(Bytes<2) return EFI_COMPROMISED_DATA;
  Max=MIN(Max,Bytes/2-1);
  CHAR16 *B=AllocateZeroPool(Bytes),*Confirm=AllocateZeroPool(Bytes); EFI_STATUS S=EFI_OUT_OF_RESOURCES;
  if(!B||!Confirm) goto Done;
  if(!Password && Q->CurrentValue.Buffer) { CopyMem(B,Q->CurrentValue.Buffer,Bytes); B[Max]=0; }
  if(Password && Q->PasswordCheck) {
    S=Q->PasswordCheck(F,Q,B);
    if(S==EFI_UNSUPPORTED||S==EFI_NOT_AVAILABLE_YET) { Message(Title,Tr("Password changes are unavailable.","当前无法修改密码。")); goto Done; }
    if(EFI_ERROR(S)) {
      S=EditText(Title,Tr("Current password","当前密码"),B,Max+1,TRUE); if(EFI_ERROR(S)) goto Done;
      S=Q->PasswordCheck(F,Q,B); if(EFI_ERROR(S)) { Message(Title,Tr("Incorrect password","密码错误")); goto Done; }
    }
    ZeroMem(B,Bytes);
  }
  for(;;) {
    S=EditText(Title,Password?Tr("New password","新密码"):Tr("Type a value. Backspace deletes the last character.","输入内容，Backspace 删除末尾字符。"),B,Max+1,Password);
    if(EFI_ERROR(S)) goto Done;
    if(StrLen(B)>=Min) break;
    Message(Title,Tr("The value is shorter than the required minimum.","输入长度小于要求的最小长度。"));
  }
  if(Password) {
    S=EditText(Title,Tr("Confirm new password","确认新密码"),Confirm,Max+1,TRUE); if(EFI_ERROR(S)) goto Done;
    if(StrCmp(B,Confirm)) { Message(Title,Tr("Passwords do not match.","两次输入的密码不一致。")); S=EFI_ABORTED; goto Done; }
  }
  V->Buffer=(UINT8 *)B; B=NULL; S=EFI_SUCCESS;
Done:
  if(EFI_ERROR(S)&&Password&&Q->PasswordCheck) Q->PasswordCheck(F,Q,NULL);
  if(B) { ZeroMem(B,Bytes); FreePool(B); }
  if(Confirm) { ZeroMem(Confirm,Bytes); FreePool(Confirm); }
  return S;
}
STATIC UINT8 *mPendingOrder;
STATIC EFI_STATUS Edit(FORM_DISPLAY_ENGINE_FORM *F,FORM_DISPLAY_ENGINE_STATEMENT *Q,USER_INPUT *U,CONST CHAR8 *Title)
{
  EFI_HII_VALUE V=Q->CurrentValue; V.Buffer=NULL; EFI_STATUS S=EFI_SUCCESS;
  switch(Q->OpCode->OpCode) {
  case EFI_IFR_REF_OP: case EFI_IFR_ACTION_OP: case EFI_IFR_RESET_BUTTON_OP: U->SelectedStatement=Q; return EFI_SUCCESS;
  case EFI_IFR_CHECKBOX_OP: V.Value.b=!V.Value.b; break;
  case EFI_IFR_ONE_OF_OP: S=PickOption(F,Q,&V,Title); break;
  case EFI_IFR_ORDERED_LIST_OP: S=OrderedList(F,Q,&V,Title); break;
  case EFI_IFR_NUMERIC_OP: S=Numeric(F,Q,&V,Title); break;
  case EFI_IFR_STRING_OP: case EFI_IFR_PASSWORD_OP: S=StringValue(F,Q,&V,Title); break;
  case EFI_IFR_DATE_OP: case EFI_IFR_TIME_OP: S=DateTime(Q,&V,Title); break;
  default: return EFI_UNSUPPORTED;
  }
  if(EFI_ERROR(S)) return S;
  if(Q->ValidateQuestion) {
    STATEMENT_ERROR_INFO E; ZeroMem(&E,sizeof(E)); UINT32 R=Q->ValidateQuestion(F,Q,&V,&E);
    if(R!=STATEMENT_VALID) {
      CHAR8 Text[2048]; HiiText(F->HiiHandle,E.StringId,Text,sizeof(Text));
      Message(Tr("Validation","校验提示"),Text[0]?Text:Tr("The value is not valid.","输入值无效。"));
      if(R!=(WARNING_IF_TRUE)) { if(Q->OpCode->OpCode==EFI_IFR_PASSWORD_OP && Q->PasswordCheck) Q->PasswordCheck(F,Q,NULL); if(V.Buffer) { ZeroMem(V.Buffer,V.BufferLen); FreePool(V.Buffer); } return EFI_ABORTED; }
    }
  }
  if(Q->OpCode->OpCode==EFI_IFR_STRING_OP || Q->OpCode->OpCode==EFI_IFR_PASSWORD_OP) {
    V.Value.string=HiiSetString(F->HiiHandle,0,(CHAR16 *)V.Buffer,NULL);
    if(!V.Value.string) { ZeroMem(V.Buffer,V.BufferLen); FreePool(V.Buffer); return EFI_OUT_OF_RESOURCES; }
  }
  /* SetupBrowser copies ordered buffers but does not release them. Keep the
     submitted allocation until the next display call, after browser callbacks. */
  if(Q->OpCode->OpCode==EFI_IFR_ORDERED_LIST_OP) mPendingOrder=V.Buffer;
  U->SelectedStatement=Q; U->InputValue=V; return EFI_SUCCESS;
}
STATIC UINTN EFIAPI ConfirmChange(VOID)
{
  if(FlangeTheme==FlangeHii) return mOriginal.ConfirmDataChange();
  FLANGE_ROW Rows[3]; ZeroMem(Rows,sizeof(Rows));
  AsciiStrCpyS(Rows[0].Label,sizeof(Rows[0].Label),Tr("Save changes","保存修改"));
  AsciiStrCpyS(Rows[1].Label,sizeof(Rows[1].Label),Tr("Discard changes","放弃修改"));
  AsciiStrCpyS(Rows[2].Label,sizeof(Rows[2].Label),Tr("Keep editing","继续编辑"));
  for(UINTN I=0;I<3;I++) Rows[I].Enabled=TRUE;
  INTN Pick=Choose(Tr("Unsaved changes","尚未保存的修改"),Tr("Choose how to handle the pending configuration.","选择如何处理尚未保存的配置。"),Rows,3,2);
  if(EFI_ERROR(mChoiceStatus)) return mOriginal.ConfirmDataChange();
  return Pick==0?BROWSER_ACTION_SUBMIT:Pick==1?BROWSER_ACTION_DISCARD:BROWSER_ACTION_NONE;
}
STATIC EFI_STATUS BrowserStatus(FORM_DISPLAY_ENGINE_FORM *F,USER_INPUT *U)
{
  CHAR8 Text[2048]; FlangeUtf16ToUtf8(F->ErrorString,Text,sizeof(Text));
  if(!Text[0]) AsciiSPrint(Text,sizeof(Text),"%s (0x%08x)",Tr("Firmware configuration requires attention","固件配置需要处理"),F->BrowserStatus);
  if(F->BrowserStatus==(BROWSER_SUBMIT_FAIL) || F->BrowserStatus==(BROWSER_SUBMIT_FAIL_NO_SUBMIT_IF) || F->BrowserStatus==(BROWSER_RECONNECT_SAVE_CHANGES)) {
    FLANGE_ROW Rows[2]; ZeroMem(Rows,sizeof(Rows));
    AsciiStrCpyS(Rows[0].Label,sizeof(Rows[0].Label),Tr("Review changes","检查修改"));
    AsciiStrCpyS(Rows[1].Label,sizeof(Rows[1].Label),Tr("Discard changes","放弃修改"));
    Rows[0].Enabled=Rows[1].Enabled=TRUE;
    U->Action=Choose(Tr("Configuration","配置"),Text,Rows,2,0)==1?BROWSER_ACTION_DISCARD:BROWSER_ACTION_GOTO;
    return EFI_SUCCESS;
  }
  U->Action=BROWSER_ACTION_NONE; return Message(Tr("Configuration notice","配置提示"),Text);
}
STATIC VOID HotKeyText(BROWSER_HOT_KEY *K,CHAR8 *Out,UINTN Size)
{
  /* The legacy engine caches these help strings. Translate standard actions
     locally so language changes do not leave mixed-language F9/F10 labels. */
  if(K->Action==BROWSER_ACTION_DEFAULT) AsciiStrCpyS(Out,Size,Tr("Restore defaults","恢复默认值"));
  else if(K->Action==BROWSER_ACTION_SUBMIT) AsciiStrCpyS(Out,Size,Tr("Save changes","保存修改"));
  else FlangeUtf16ToUtf8(K->HelpString,Out,Size);
}
STATIC BOOLEAN ConfirmHotKey(BROWSER_HOT_KEY *K)
{
  if(!(K->Action&(BROWSER_ACTION_DEFAULT|BROWSER_ACTION_DISCARD|BROWSER_ACTION_RESET|BROWSER_ACTION_EXIT|BROWSER_ACTION_SUBMIT))) return TRUE;
  FLANGE_ROW R[2]; ZeroMem(R,sizeof(R));
  AsciiStrCpyS(R[0].Label,sizeof(R[0].Label),Tr("Apply","应用"));
  AsciiStrCpyS(R[1].Label,sizeof(R[1].Label),Tr("Cancel","取消"));
  R[0].Enabled=R[1].Enabled=TRUE;
  CHAR8 Title[256]; HotKeyText(K,Title,sizeof(Title));
  return Choose(Title,Tr("Apply this action to the current configuration?","将此操作应用到当前配置？"),R,2,1)==0;
}
STATIC EFI_STATUS EFIAPI FormDisplay(FORM_DISPLAY_ENGINE_FORM *F,USER_INPUT *U)
{
  if(mPendingOrder) { FreePool(mPendingOrder); mPendingOrder=NULL; }
  if(FlangeTheme==FlangeHii) return mOriginal.FormDisplay(F,U);
  ZeroMem(U,sizeof(*U));
  if(F->BrowserStatus!=BROWSER_SUCCESS) return BrowserStatus(F,U);
  FLANGE_PANEL P; InitPanel(&P,""); HiiText(F->HiiHandle,F->FormTitle,P.Title,sizeof(P.Title));
  P.Count=CountRows(F,&F->StatementListHead)+CountRows(F,&F->StatementListOSF);
  P.Rows=AllocateZeroPool(MAX(1,P.Count)*sizeof(*P.Rows));
  FORM_DISPLAY_ENGINE_STATEMENT **Map=AllocateZeroPool(MAX(1,P.Count)*sizeof(*Map));
  if(!P.Rows||!Map) { if(P.Rows) FreePool(P.Rows); if(Map) FreePool(Map); return EFI_OUT_OF_RESOURCES; }
  UINTN Index=0;
  FillRows(F,&F->StatementListOSF,&P,Map,&Index,F->Attribute);
  FillRows(F,&F->StatementListHead,&P,Map,&Index,F->Attribute);
  if(P.Count && P.Rows[P.Selected].Group) {
    for(UINTN I=0;I<P.Count;I++) if(P.Rows[I].Enabled) { P.Selected=I; break; }
  }
  AsciiStrCpyS(P.Description,sizeof(P.Description),Tr("Select an item to view its details. Changed values are marked at the left edge.","选择项目查看说明，已修改的值在左侧显示标记。"));
  LIST_ENTRY *L;
  for(L=GetFirstNode(&F->HotKeyListHead);!IsNull(&F->HotKeyListHead,L);L=GetNextNode(&F->HotKeyListHead,L)) {
    BROWSER_HOT_KEY *K=BROWSER_HOT_KEY_FROM_LINK(L); CHAR8 Text[128],Label[160];
    HotKeyText(K,Text,sizeof(Text));
    if(K->KeyData->ScanCode>=SCAN_F1 && K->KeyData->ScanCode<=SCAN_F12) AsciiSPrint(Label,sizeof(Label),"F%u %a   ",K->KeyData->ScanCode-SCAN_F1+1,Text);
    else AsciiSPrint(Label,sizeof(Label),"%c %a   ",K->KeyData->UnicodeChar,Text);
    if(AsciiStrLen(P.Footer)+AsciiStrLen(Label)<sizeof(P.Footer)) AsciiStrCatS(P.Footer,sizeof(P.Footer),Label);
  }
  EFI_STATUS S;
  for(;;) {
    S=FlangeRunPanel(&P,F->FormRefreshEvent);
    if(S==EFI_MEDIA_CHANGED) { U->Action=BROWSER_ACTION_NONE; S=EFI_SUCCESS; break; }
    if(EFI_ERROR(S)) break;
    if(P.Action==PanelBack) { if(F->Attribute&HII_DISPLAY_MODAL) continue; U->Action=BROWSER_ACTION_FORM_EXIT; break; }
    if(P.Action==PanelKey) {
      for(L=GetFirstNode(&F->HotKeyListHead);!IsNull(&F->HotKeyListHead,L);L=GetNextNode(&F->HotKeyListHead,L)) {
        BROWSER_HOT_KEY *K=BROWSER_HOT_KEY_FROM_LINK(L);
        UINT32 Key=K->KeyData->ScanCode?FLANGE_SCAN(K->KeyData->ScanCode):K->KeyData->UnicodeChar;
        if(Key==P.Key && !(F->Attribute&HII_DISPLAY_LOCK) && ConfirmHotKey(K)) { U->Action=K->Action; U->DefaultId=K->DefaultId; break; }
      }
      if(U->Action) break;
    }
    if(P.Action==PanelSelect && P.Count && P.Rows[P.Selected].Enabled) {
      S=Edit(F,Map[P.Selected],U,P.Rows[P.Selected].Label);
      if(!EFI_ERROR(S)) break;
      if(S!=EFI_ABORTED) Message(Tr("Unable to edit","无法编辑"),Tr("This setting could not be updated.","无法更新该设置。"));
    }
  }
  FreePool(P.Rows); FreePool(Map);
  if(EFI_ERROR(S)) return mOriginal.FormDisplay(F,U); /* serial/headless recovery */
  return S;
}
STATIC VOID EFIAPI ExitDisplay(VOID)
{
  if(mPendingOrder) { FreePool(mPendingOrder); mPendingOrder=NULL; }
  mOriginal.ExitDisplay();
}
EFI_STATUS FlangeFormsInstall(EFI_HANDLE Image)
{
  (VOID)Image;
  FLANGE_UI_PROTOCOL *Existing;
  if(mInstalled || !EFI_ERROR(gBS->LocateProtocol(&mServiceGuid,NULL,(VOID **)&Existing))) return EFI_ALREADY_STARTED;
  EFI_STATUS S=gBS->LocateProtocol(&gEdkiiFormDisplayEngineProtocolGuid,NULL,(VOID **)&mEngine);
  if(EFI_ERROR(S)) return S;
  S=gBS->InstallProtocolInterface(&mServiceHandle,&mServiceGuid,EFI_NATIVE_INTERFACE,&mService);
  if(EFI_ERROR(S)) return S;
  mOriginal=*mEngine; mEngine->FormDisplay=FormDisplay; mEngine->ExitDisplay=ExitDisplay; mEngine->ConfirmDataChange=ConfirmChange;
  if(!EFI_ERROR(gBS->LocateProtocol(&gEfiHiiPopupProtocolGuid,NULL,(VOID **)&mPopup))) { mOriginalPopup=*mPopup; mPopup->CreatePopup=Popup; }
  mInstalled=TRUE; return EFI_SUCCESS;
}
VOID FlangeFormsRemove(VOID)
{
  if(!mInstalled) return;
  *mEngine=mOriginal; if(mPopup) *mPopup=mOriginalPopup;
  gBS->UninstallProtocolInterface(mServiceHandle,&mServiceGuid,&mService); mServiceHandle=NULL; mInstalled=FALSE;
  if(mPendingOrder) { FreePool(mPendingOrder); mPendingOrder=NULL; }
}
