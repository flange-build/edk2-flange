/** @file
  The flange boot logo, as an EDKII Platform Logo protocol.

  The logo comes in a few widths, and the largest one that takes up no more
  than two fifths of the screen width is shown, centred.

  Copyright (c) 2016 - 2017, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiDatabase.h>
#include <Protocol/HiiImageEx.h>
#include <Protocol/HiiPackageList.h>
#include <Protocol/PlatformLogo.h>

//
// Largest first.
//
STATIC CONST EFI_IMAGE_ID  mLogoSizes[] = {
  IMAGE_TOKEN (IMG_LOGO_1152),
  IMAGE_TOKEN (IMG_LOGO_768),
  IMAGE_TOKEN (IMG_LOGO_384)
};

//
// Shown for a screen of unknown size.
//
#define LOGO_DEFAULT_SIZE  1

STATIC EFI_HII_IMAGE_EX_PROTOCOL  *mHiiImageEx;
STATIC EFI_HII_HANDLE             mHiiHandle;

/**
  Returns the logo that suits the console's screen.

  @return  The image ID.
**/
STATIC
EFI_IMAGE_ID
PickLogo (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *GraphicsOutput;
  EFI_IMAGE_OUTPUT              Info;
  UINTN                         Index;

  Status = gBS->HandleProtocol (
                  gST->ConsoleOutHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&GraphicsOutput
                  );
  if (EFI_ERROR (Status)) {
    return mLogoSizes[LOGO_DEFAULT_SIZE];
  }

  for (Index = 0; Index < ARRAY_SIZE (mLogoSizes) - 1; Index++) {
    Status = mHiiImageEx->GetImageInfo (mHiiImageEx, mHiiHandle, mLogoSizes[Index], &Info);
    if (!EFI_ERROR (Status) &&
        ((UINTN)Info.Width * 5 <= (UINTN)GraphicsOutput->Mode->Info->HorizontalResolution * 2) &&
        (Info.Height <= GraphicsOutput->Mode->Info->VerticalResolution))
    {
      break;
    }
  }

  return mLogoSizes[Index];
}

/**
  Load a platform logo image and return its data and attributes.

  @param This              The pointer to this protocol instance.
  @param Instance          The visible image instance is found.
  @param Image             Points to the image.
  @param Attribute         The display attributes of the image returned.
  @param OffsetX           The X offset of the image regarding the Attribute.
  @param OffsetY           The Y offset of the image regarding the Attribute.

  @retval EFI_SUCCESS      The image was fetched successfully.
  @retval EFI_NOT_FOUND    The specified image could not be found.
**/
STATIC
EFI_STATUS
EFIAPI
GetImage (
  IN     EDKII_PLATFORM_LOGO_PROTOCOL        *This,
  IN OUT UINT32                              *Instance,
  OUT EFI_IMAGE_INPUT                        *Image,
  OUT EDKII_PLATFORM_LOGO_DISPLAY_ATTRIBUTE  *Attribute,
  OUT INTN                                   *OffsetX,
  OUT INTN                                   *OffsetY
  )
{
  if ((Instance == NULL) || (Image == NULL) ||
      (Attribute == NULL) || (OffsetX == NULL) || (OffsetY == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (*Instance != 0) {
    return EFI_NOT_FOUND;
  }

  (*Instance)++;
  *Attribute = EdkiiPlatformLogoDisplayAttributeCenter;
  *OffsetX   = 0;
  *OffsetY   = 0;

  return mHiiImageEx->GetImageEx (mHiiImageEx, mHiiHandle, PickLogo (), Image);
}

STATIC EDKII_PLATFORM_LOGO_PROTOCOL  mPlatformLogo = {
  GetImage
};

/**
  Entrypoint of this module.

  This function is the entrypoint of this module. It installs the Edkii
  Platform Logo protocol.

  @param  ImageHandle       The firmware allocated handle for the EFI image.
  @param  SystemTable       A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.

**/
EFI_STATUS
EFIAPI
InitializeLogo (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_HII_PACKAGE_LIST_HEADER  *PackageList;
  EFI_HII_DATABASE_PROTOCOL    *HiiDatabase;
  EFI_HANDLE                   Handle;

  Status = gBS->LocateProtocol (
                  &gEfiHiiDatabaseProtocolGuid,
                  NULL,
                  (VOID **)&HiiDatabase
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->LocateProtocol (
                  &gEfiHiiImageExProtocolGuid,
                  NULL,
                  (VOID **)&mHiiImageEx
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Retrieve HII package list from ImageHandle
  //
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiHiiPackageListProtocolGuid,
                  (VOID **)&PackageList,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "HII Image Package with logo not found in PE/COFF resource section\n"));
    return Status;
  }

  //
  // Publish HII package list to HII Database.
  //
  Status = HiiDatabase->NewPackageList (
                          HiiDatabase,
                          PackageList,
                          NULL,
                          &mHiiHandle
                          );
  if (!EFI_ERROR (Status)) {
    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gEdkiiPlatformLogoProtocolGuid,
                    &mPlatformLogo,
                    NULL
                    );
  }

  return Status;
}
