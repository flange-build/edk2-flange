/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_FORMS_H
#define FLANGE_FORMS_H
#include <Uefi.h>
#include "FlangePanel.h"
EFI_STATUS FlangeRunPanel(FLANGE_PANEL *Panel, EFI_EVENT Refresh);
EFI_STATUS FlangeFormsInstall(EFI_HANDLE Image);
VOID FlangeFormsRemove(VOID);
VOID FlangeThemeLoad(VOID);
EFI_STATUS EFIAPI FlangeChooseTheme(VOID);
BOOLEAN FlangeIsChinese(VOID);
#endif
