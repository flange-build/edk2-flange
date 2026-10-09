/** @file
  LVGL boot-services application shared by RK3588 and QCS6490.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/SimplePointer.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "FlangeView.h"
#include "FlangeText.h"
#include "FlangeForms.h"

STATIC EFI_GUID mUiGuid = {0x80c6b7d4,0x83d8,0x4941,{0xab,0x92,0xb6,0xb5,0xd2,0xda,0x54,0x35}};
STATIC EFI_GUID mLegacyGuid = {0x462caa21,0x7614,0x4503,{0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_SIMPLE_POINTER_PROTOCOL *mPointer;
STATIC BOOLEAN mBltFailed;
STATIC UINT32 mMenuMode;
STATIC BOOLEAN mHaveMenuMode;
STATIC INT32 mMouseX, mMouseY;
STATIC BOOLEAN mMouseDown;
STATIC UINT64 mTickOrigin, mTickStart, mTickEnd, mTickFrequency;

STATIC uint32_t Tick(VOID)
{
  UINT64 Now = GetPerformanceCounter();
  UINT64 Delta;
  if(mTickStart < mTickEnd) {
    Delta = Now >= mTickOrigin ? Now - mTickOrigin : (mTickEnd - mTickOrigin) + (Now - mTickStart) + 1;
  } else {
    Delta = Now <= mTickOrigin ? mTickOrigin - Now : (mTickOrigin - mTickEnd) + (mTickStart - Now) + 1;
  }
  /* Quotient first avoids overflow after a long firmware session. */
  return (uint32_t)((Delta / mTickFrequency) * 1000 + ((Delta % mTickFrequency) * 1000) / mTickFrequency);
}
STATIC VOID RestoreDisplayMode(VOID)
{
  /* UiApp and returning EFI applications can change GOP resolution. The next
   * LVGL session must use the display mode that was active on entry. */
  if(mHaveMenuMode && mGop && mGop->Mode && mGop->Mode->Mode != mMenuMode) {
    mGop->SetMode(mGop, mMenuMode);
  }
}
STATIC void Flush(lv_display_t *Display, const lv_area_t *Area, uint8_t *Pixels)
{
  EFI_STATUS Status;
  UINTN Width = (UINTN)(Area->x2 - Area->x1 + 1);
  Status = mGop->Blt(mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Pixels,
                    EfiBltBufferToVideo, 0, 0, (UINTN)Area->x1, (UINTN)Area->y1,
                    Width, (UINTN)(Area->y2 - Area->y1 + 1), Width * 4);
  if(EFI_ERROR(Status)) mBltFailed = TRUE;
  lv_display_flush_ready(Display);
}
STATIC void ReadPointer(lv_indev_t *Input, lv_indev_data_t *Data)
{
  EFI_SIMPLE_POINTER_STATE State;
  (VOID)Input;
  if(!EFI_ERROR(mPointer->GetState(mPointer, &State))) {
    /* GetState is relative; at least one pixel per nonzero report. */
    INT64 X = State.RelativeMovementX;
    INT64 Y = State.RelativeMovementY;
    if(mPointer->Mode->ResolutionX) X = X * 16 / (INT64)mPointer->Mode->ResolutionX;
    if(mPointer->Mode->ResolutionY) Y = Y * 16 / (INT64)mPointer->Mode->ResolutionY;
    if(!X && State.RelativeMovementX) X = State.RelativeMovementX > 0 ? 1 : -1;
    if(!Y && State.RelativeMovementY) Y = State.RelativeMovementY > 0 ? 1 : -1;
    mMouseX = (INT32)MAX(0, MIN((INT64)mGop->Mode->Info->HorizontalResolution - 1, mMouseX + X));
    mMouseY = (INT32)MAX(0, MIN((INT64)mGop->Mode->Info->VerticalResolution - 1, mMouseY + Y));
    mMouseDown = State.LeftButton;
  }
  Data->point.x = mMouseX;
  Data->point.y = mMouseY;
  Data->state = mMouseDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
STATIC BOOLEAN IsMenu(EFI_DEVICE_PATH_PROTOCOL *Path)
{
  EFI_GUID *Guid;
  for(; !IsDevicePathEnd(Path); Path = NextDevicePathNode(Path)) {
    Guid = EfiGetNameGuidFromFwVolDevicePathNode((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Path);
    if(Guid && (CompareGuid(Guid, &mUiGuid) || CompareGuid(Guid, &mLegacyGuid))) return TRUE;
  }
  return FALSE;
}
STATIC EFI_STATUS LegacyMenu(EFI_HANDLE ImageHandle)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count, Index;
  EFI_STATUS Status;
  EFI_HANDLE Child = NULL;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles);
  if(EFI_ERROR(Status)) return Status;
  Status = EFI_NOT_FOUND;
  for(Index = 0; Index < Count; Index++) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
    EFI_FV_FILETYPE Type;
    EFI_FV_FILE_ATTRIBUTES Attributes;
    UINT32 Authentication;
    UINTN Size = 0;
    EFI_DEVICE_PATH_PROTOCOL *Base, *Path;
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Node;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) continue;
    /* Metadata query: no executable buffer is allocated or trusted here. */
    if(EFI_ERROR(Fv->ReadFile(Fv, &mLegacyGuid, NULL, &Size, &Type, &Attributes, &Authentication))) continue;
    Base = DevicePathFromHandle(Handles[Index]);
    if(!Base) continue;
    EfiInitializeFwVolDevicepathNode(&Node, &mLegacyGuid);
    Path = AppendDevicePathNode(Base, (EFI_DEVICE_PATH_PROTOCOL *)&Node);
    if(!Path) { Status = EFI_OUT_OF_RESOURCES; break; }
    /* LoadImage performs the normal firmware image authentication. */
    Status = gBS->LoadImage(FALSE, ImageHandle, Path, NULL, 0, &Child);
    FreePool(Path);
    if(!EFI_ERROR(Status)) break;
    /* EFI_SECURITY_VIOLATION may still return an image handle. */
    if(Child) { gBS->UnloadImage(Child); Child = NULL; }
  }
  FreePool(Handles);
  if(!EFI_ERROR(Status)) {
    Status = gBS->StartImage(Child, NULL, NULL);
    gBS->UnloadImage(Child);
  }
  return Status;
}
STATIC VOID ReadSettings(FLANGE_MODEL *Model)
{
  CHAR8 Language[64];
  UINT8 Secure;
  UINTN Size = sizeof(Language);
  Model->Chinese = !EFI_ERROR(gRT->GetVariable(L"PlatformLang", &gEfiGlobalVariableGuid, NULL, &Size, Language)) && Size >= 2 && Language[0]=='z' && Language[1]=='h';
  Size = sizeof(Secure);
  Model->SecureBoot = EFI_ERROR(gRT->GetVariable(L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &Size, &Secure)) ? -1 : Secure != 0;
}
STATIC EFI_STATUS GetEntries(FLANGE_MODEL *Model, EFI_BOOT_MANAGER_LOAD_OPTION **Options, UINTN *Count, UINTN **Map)
{
  UINTN Index;
  CHAR16 *Path;
  *Options = EfiBootManagerGetLoadOptions(Count, LoadOptionTypeBoot);
  Model->Count = 0;
  Model->Entries = NULL;
  *Map = NULL;
  if(!*Options || !*Count) return EFI_SUCCESS;
  Model->Entries = AllocateZeroPool(*Count * sizeof(*Model->Entries));
  *Map = AllocatePool(*Count * sizeof(**Map));
  if(!Model->Entries || !*Map) return EFI_OUT_OF_RESOURCES;
  for(Index=0; Index<*Count; Index++) {
    EFI_BOOT_MANAGER_LOAD_OPTION *Option = &(*Options)[Index];
    if(!(Option->Attributes & LOAD_OPTION_ACTIVE) || (Option->Attributes & LOAD_OPTION_HIDDEN) ||
       !Option->FilePath || IsMenu(Option->FilePath)) continue;
    FLANGE_ENTRY *Entry = &Model->Entries[Model->Count];
    Entry->Number = (UINT16)Option->OptionNumber;
    FlangeUtf16ToUtf8(Option->Description, Entry->Title, sizeof(Entry->Title));
    Path = ConvertDevicePathToText(Option->FilePath, FALSE, FALSE);
    if(Path) { FlangeUtf16ToUtf8(Path, Entry->Path, sizeof(Entry->Path)); FreePool(Path); }
    (*Map)[Model->Count++] = Index;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS RunSession(VOID *Context, VOID (*Create)(VOID *), VOID (*KeyInput)(VOID *,UINT32), bool (*Poll)(VOID *), EFI_EVENT Refresh)
{
  EFI_STATUS Status;
  EFI_EVENT Timer = NULL;
  EFI_EVENT Events[2];
  UINTN EventCount, EventIndex;
  VOID *Buffer = NULL;
  lv_display_t *Display;
  EFI_INPUT_KEY Key;
  BOOLEAN CursorVisible = gST->ConOut->Mode->CursorVisible;
  RestoreDisplayMode();
  Status = gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop);
  if(EFI_ERROR(Status)) Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if(EFI_ERROR(Status) || !mGop->Mode || !mGop->Mode->Info) return EFI_UNSUPPORTED;
  UINT32 Width = mGop->Mode->Info->HorizontalResolution;
  UINT32 Height = mGop->Mode->Info->VerticalResolution;
  /* Preserve the platform's display mode and framebuffer handoff. */
  if(Width < 800 || Height < 600 || Width > 7680 || Height > 4320 || !gST->ConIn) return EFI_UNSUPPORTED;
  Buffer = AllocatePool((UINTN)Width * 64 * 4);
  if(!Buffer) return EFI_OUT_OF_RESOURCES;
  Status = gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL, &Timer);
  if(EFI_ERROR(Status)) { FreePool(Buffer); return Status; }
  Status = gBS->SetTimer(Timer, TimerPeriodic, 100000); /* 10 ms; no busy loop. */
  if(EFI_ERROR(Status)) { gBS->CloseEvent(Timer); FreePool(Buffer); return Status; }
  mTickFrequency = GetPerformanceCounterProperties(&mTickStart, &mTickEnd);
  if(!mTickFrequency) { gBS->CloseEvent(Timer); FreePool(Buffer); return EFI_UNSUPPORTED; }
  mTickOrigin = GetPerformanceCounter();
  mBltFailed = FALSE;
  mMenuMode = mGop->Mode->Mode;
  mHaveMenuMode = TRUE;
  lv_init();
  lv_tick_set_cb(Tick);
  Display = lv_display_create((int32_t)Width, (int32_t)Height);
  if(!Display) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  lv_display_set_color_format(Display, LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_buffers(Display, Buffer, NULL, Width * 64 * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(Display, Flush);
  gST->ConOut->EnableCursor(gST->ConOut, FALSE);
  Create(Context);
  mPointer = NULL;
  Status = gBS->HandleProtocol(gST->ConsoleInHandle, &gEfiSimplePointerProtocolGuid, (VOID **)&mPointer);
  if(EFI_ERROR(Status)) gBS->LocateProtocol(&gEfiSimplePointerProtocolGuid, NULL, (VOID **)&mPointer);
  if(mPointer && mPointer->Mode) {
    mMouseX = (INT32)Width/2; mMouseY = (INT32)Height/2; mMouseDown = FALSE;
    lv_indev_t *Pointer = lv_indev_create();
    if(Pointer) {
      lv_indev_set_type(Pointer, LV_INDEV_TYPE_POINTER);
      lv_indev_set_read_cb(Pointer, ReadPointer);
      lv_obj_t *Cursor = lv_label_create(lv_layer_top());
      lv_label_set_text(Cursor, "+");
      lv_obj_set_style_text_color(Cursor, lv_color_hex(FlangePalette()->Ink), 0);
      lv_indev_set_cursor(Pointer, Cursor);
    }
  }
  Events[0] = Timer;
  EventCount = 1;
  if(gST->ConIn->WaitForKey) Events[EventCount++] = gST->ConIn->WaitForKey;
  Status = EFI_SUCCESS;
  while(!mBltFailed) {
    lv_timer_handler();
    if(Poll(Context) || mBltFailed) break;
    if(Refresh && !EFI_ERROR(gBS->CheckEvent(Refresh))) { Status = EFI_MEDIA_CHANGED; break; }
    Status = gBS->WaitForEvent(EventCount, Events, &EventIndex);
    if(EFI_ERROR(Status)) break;
    /* Consume one key per frame so held keys cannot starve rendering. */
    if(!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &Key))) {
      KeyInput(Context, Key.ScanCode ? FLANGE_SCAN(Key.ScanCode) : Key.UnicodeChar);
    }
  }
  if(mBltFailed) Status = EFI_DEVICE_ERROR;
Done:
  lv_deinit();
  gBS->CloseEvent(Timer);
  FreePool(Buffer);
  if(Create != FlangePanelCreate || !((FLANGE_PANEL *)Context)->ReadOnly) gST->ConOut->ClearScreen(gST->ConOut);
  gST->ConOut->EnableCursor(gST->ConOut, CursorVisible);
  return Status;
}
typedef struct { FLANGE_MODEL *Model; FLANGE_ACTION *Action; } BOOT_CONTEXT;
STATIC VOID BootCreate(VOID *Context) { FlangeViewCreate(((BOOT_CONTEXT *)Context)->Model); }
STATIC bool BootPoll(VOID *Context) { BOOT_CONTEXT *C=Context; *C->Action=FlangeViewPoll(); return *C->Action!=FlangeNone; }
STATIC VOID BootKey(VOID *Context, UINT32 K)
{
  (VOID)Context;
  switch(K) {
  case FLANGE_SCAN_UP: K=LV_KEY_UP; break;
  case FLANGE_SCAN_DOWN: K=LV_KEY_DOWN; break;
  case FLANGE_SCAN_PGUP: K=LV_KEY_PREV; break;
  case FLANGE_SCAN_PGDN: K=LV_KEY_NEXT; break;
  case FLANGE_SCAN_HOME: K=LV_KEY_HOME; break;
  case FLANGE_SCAN_END: K=LV_KEY_END; break;
  case FLANGE_SCAN(SCAN_F2): K='s'; break;
  case FLANGE_SCAN_ESC: K=LV_KEY_ESC; break;
  default: break;
  }
  FlangeViewKey(K);
}
STATIC EFI_STATUS RunView(FLANGE_MODEL *Model, FLANGE_ACTION *Action)
{
  BOOT_CONTEXT C={Model,Action}; *Action=FlangeNone;
  return RunSession(&C,BootCreate,BootKey,BootPoll,NULL);
}
EFI_STATUS FlangeRunPanel(FLANGE_PANEL *Panel, EFI_EVENT Refresh)
{
  return RunSession(Panel,FlangePanelCreate,FlangePanelKey,FlangePanelPoll,Refresh);
}
EFI_STATUS EFIAPI FlangeUiEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  FLANGE_MODEL Model;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options;
  UINTN Count, *Map;
  FLANGE_ACTION Action;
  EFI_STATUS Status;
  CHAR8 Message[256] = {0};
  UINT16 SelectedNumber = MAX_UINT16;
  (VOID)SystemTable;
  /* Matches the existing boot-manager UI's device discovery semantics. */
  EfiBootManagerConnectAll();
  EfiBootManagerRefreshAllBootOption();
  if(!EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&mGop)) && mGop->Mode) {
    mMenuMode=mGop->Mode->Mode; mHaveMenuMode=TRUE;
  }
  FlangeThemeLoad();
  Status=FlangeFormsInstall(ImageHandle);
  if(Status==EFI_ALREADY_STARTED) return Status;
  if(EFI_ERROR(Status)) return LegacyMenu(ImageHandle);
  for(;;) {
    if(FlangeTheme == FlangeHii) {
      Status = LegacyMenu(ImageHandle);
      if(FlangeTheme == FlangeHii || EFI_ERROR(Status)) { FlangeFormsRemove(); return Status; }
    }
    ZeroMem(&Model, sizeof(Model));
    Count = 0; Options = NULL; Map = NULL;
    ReadSettings(&Model);
    Model.Status = Message[0] ? Message : NULL;
    Status = GetEntries(&Model, &Options, &Count, &Map);
    if(!EFI_ERROR(Status)) {
      for(UINTN Index=0; Index<Model.Count; Index++) {
        if(Model.Entries[Index].Number == SelectedNumber) Model.Selected = Index;
      }
      Status = RunView(&Model, &Action);
    }
    if(EFI_ERROR(Status)) Action = FlangeSetup;
    if(Model.Count) SelectedNumber = Model.Entries[Model.Selected].Number;
    Message[0] = 0;
    if(Action == FlangeBoot && Model.Count) {
      EFI_BOOT_MANAGER_LOAD_OPTION *Option = &Options[Map[Model.Selected]];
      EfiBootManagerBoot(Option);
      RestoreDisplayMode();
      if(EFI_ERROR(Option->Status)) {
        AsciiSPrint(Message, sizeof(Message), "Boot%04x: %r", Option->OptionNumber, Option->Status);
      } else {
        AsciiStrCpyS(Message, sizeof(Message), Model.Chinese ? "启动程序已返回" : "Boot application returned");
      }
    }
    if(Model.Entries) FreePool(Model.Entries);
    if(Map) FreePool(Map);
    if(Options) EfiBootManagerFreeLoadOptions(Options, Count);
    if(Action == FlangeExit) { FlangeFormsRemove(); return EFI_SUCCESS; }
    if(Action == FlangeThemes) FlangeChooseTheme();
    if(Action == FlangeSetup) {
      EFI_STATUS LegacyStatus = LegacyMenu(ImageHandle);
      if(EFI_ERROR(Status)) { FlangeFormsRemove(); return LegacyStatus; } /* Headless fallback is one-way. */
      RestoreDisplayMode();
      if(EFI_ERROR(LegacyStatus)) AsciiSPrint(Message, sizeof(Message), "Settings: %r", LegacyStatus);
    }
    if(Action == FlangeLanguage) {
      CHAR8 *Language = Model.Chinese ? "en-US" : "zh-Hans";
      Status = gRT->SetVariable(L"PlatformLang", &gEfiGlobalVariableGuid,
                               EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                               AsciiStrSize(Language), Language);
      if(EFI_ERROR(Status)) AsciiSPrint(Message, sizeof(Message), "Language: %r", Status);
    }
    if(Action == FlangeRefresh || Action == FlangeSetup) {
      EfiBootManagerConnectAll();
      EfiBootManagerRefreshAllBootOption();
    }
  }
}
