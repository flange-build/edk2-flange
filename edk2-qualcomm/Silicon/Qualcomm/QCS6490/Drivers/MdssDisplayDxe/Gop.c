/** @file
  Graphics Output Protocol over the MDSS framebuffer: one mode, the one the
  display was brought up in.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Gop.h"

#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/GraphicsOutput.h>

#define DISPLAY_GOP_VENDOR_GUID \
  { 0xe7599d00, 0xf489, 0x48d7, { 0xba, 0x02, 0xdb, 0x7b, 0xd6, 0xde, 0xbb, 0x2d } }

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DISPLAY_DEVICE_PATH;
#pragma pack ()

STATIC DISPLAY_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    DISPLAY_GOP_VENDOR_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC FRAME_BUFFER_CONFIGURE                *mBltConfig;

STATIC EFI_EDID_DISCOVERED_PROTOCOL  mEdidDiscovered;
STATIC EFI_EDID_ACTIVE_PROTOCOL      mEdidActive;

/**
  Returns information about a mode.

  @param[in]   This          The protocol.
  @param[in]   ModeNumber    The mode.
  @param[out]  SizeOfInfo    The size of Info.
  @param[out]  Info          The information, allocated from pool.

  @retval EFI_SUCCESS            Returned.
  @retval EFI_INVALID_PARAMETER  No such mode.
  @retval EFI_OUT_OF_RESOURCES   No memory.
**/
STATIC
EFI_STATUS
EFIAPI
DisplayGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((SizeOfInfo == NULL) || (Info == NULL) || (ModeNumber >= mMode.MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mModeInfo);

  return EFI_SUCCESS;
}

/**
  Sets a mode, and clears the screen to black.

  @param[in]  This        The protocol.
  @param[in]  ModeNumber  The mode.

  @retval EFI_SUCCESS      Set.
  @retval EFI_UNSUPPORTED  No such mode.
**/
STATIC
EFI_STATUS
EFIAPI
DisplayGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if (ModeNumber >= mMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Black, sizeof (Black));

  return FrameBufferBlt (
           mBltConfig,
           &Black,
           EfiBltVideoFill,
           0,
           0,
           0,
           0,
           mModeInfo.HorizontalResolution,
           mModeInfo.VerticalResolution,
           0
           );
}

/**
  Performs a Blt operation on the framebuffer.

  @param[in]      This          The protocol.
  @param[in,out]  BltBuffer     The data.
  @param[in]      BltOperation  The operation.
  @param[in]      SourceX       Source X.
  @param[in]      SourceY       Source Y.
  @param[in]      DestinationX  Destination X.
  @param[in]      DestinationY  Destination Y.
  @param[in]      Width         Width.
  @param[in]      Height        Height.
  @param[in]      Delta         Bytes per line of BltBuffer, 0 for Width.

  @return  The status of FrameBufferBlt.
**/
STATIC
EFI_STATUS
EFIAPI
DisplayGopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta         OPTIONAL
  )
{
  return FrameBufferBlt (
           mBltConfig,
           BltBuffer,
           BltOperation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
}

/**
  Installs a Graphics Output Protocol for a running framebuffer on a new
  handle with a vendor hardware device path, plus the EDID protocols when
  there is an EDID.

  @param[in]   FrameBufferBase  The framebuffer.
  @param[in]   FrameBufferSize  Its size in bytes.
  @param[in]   Timing           The mode it is scanned out in.
  @param[in]   Edid             The sink's EDID, or NULL.
  @param[in]   EdidSize         Its size.
  @param[out]  Handle           The new handle.

  @retval EFI_SUCCESS  Installed.
  @retval Other        Failed; nothing is installed.
**/
EFI_STATUS
DisplayGopInstall (
  IN  EFI_PHYSICAL_ADDRESS  FrameBufferBase,
  IN  UINTN                 FrameBufferSize,
  IN  CONST DISPLAY_TIMING  *Timing,
  IN  CONST UINT8           *Edid        OPTIONAL,
  IN  UINTN                 EdidSize,
  OUT EFI_HANDLE            *Handle
  )
{
  EFI_STATUS  Status;
  UINTN       BltConfigSize;

  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Timing->HActive;
  mModeInfo.VerticalResolution   = Timing->VActive;
  mModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine    = Timing->HActive;

  mMode.MaxMode         = 1;
  mMode.Mode            = 0;
  mMode.Info            = &mModeInfo;
  mMode.SizeOfInfo      = sizeof (mModeInfo);
  mMode.FrameBufferBase = FrameBufferBase;
  mMode.FrameBufferSize = FrameBufferSize;

  mGop.QueryMode = DisplayGopQueryMode;
  mGop.SetMode   = DisplayGopSetMode;
  mGop.Blt       = DisplayGopBlt;
  mGop.Mode      = &mMode;

  BltConfigSize = 0;
  Status        = FrameBufferBltConfigure (
                    (VOID *)(UINTN)FrameBufferBase,
                    &mModeInfo,
                    NULL,
                    &BltConfigSize
                    );
  if (Status != RETURN_BUFFER_TOO_SMALL) {
    return EFI_UNSUPPORTED;
  }

  mBltConfig = AllocatePool (BltConfigSize);
  if (mBltConfig == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)FrameBufferBase,
             &mModeInfo,
             mBltConfig,
             &BltConfigSize
             );
  if (EFI_ERROR (Status)) {
    goto FreeBltConfig;
  }

  *Handle = NULL;
  Status  = gBS->InstallMultipleProtocolInterfaces (
                   Handle,
                   &gEfiDevicePathProtocolGuid,
                   &mDevicePath,
                   &gEfiGraphicsOutputProtocolGuid,
                   &mGop,
                   NULL
                   );
  if (EFI_ERROR (Status)) {
    goto FreeBltConfig;
  }

  if ((Edid != NULL) && (EdidSize != 0)) {
    mEdidDiscovered.SizeOfEdid = (UINT32)EdidSize;
    mEdidDiscovered.Edid       = (UINT8 *)Edid;
    mEdidActive.SizeOfEdid     = (UINT32)EdidSize;
    mEdidActive.Edid           = (UINT8 *)Edid;

    Status = gBS->InstallMultipleProtocolInterfaces (
                    Handle,
                    &gEfiEdidDiscoveredProtocolGuid,
                    &mEdidDiscovered,
                    &gEfiEdidActiveProtocolGuid,
                    &mEdidActive,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "%a: EDID protocols not installed: %r\n", __func__, Status));
    }
  }

  return EFI_SUCCESS;

FreeBltConfig:
  FreePool (mBltConfig);
  mBltConfig = NULL;

  return Status;
}
