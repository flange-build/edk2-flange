/** @file
  Graphics Output Protocol over the MDSS framebuffer.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef DISPLAY_GOP_H_
#define DISPLAY_GOP_H_

#include "MdssDisplay.h"

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
  );

#endif // DISPLAY_GOP_H_
