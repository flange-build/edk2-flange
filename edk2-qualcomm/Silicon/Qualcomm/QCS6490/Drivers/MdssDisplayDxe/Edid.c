/** @file
  Just enough EDID parsing to log what is connected and to tell an HDMI sink
  from a DVI one.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"

#define EDID_BLOCK_SIZE           128
#define EDID_MANUFACTURER         8
#define EDID_PRODUCT              10
#define EDID_VERSION              18
#define EDID_REVISION             19
#define EDID_DESCRIPTOR(n)        (54 + 18 * (n))
#define EDID_DESCRIPTOR_COUNT     4
#define EDID_DESCRIPTOR_NAME      0xFC
#define EDID_EXTENSION_COUNT      126

#define CEA_EXTENSION_TAG         0x02
#define CEA_DTD_OFFSET            2
#define CEA_DATA_BLOCKS           4
#define CEA_BLOCK_TAG(b)          ((b) >> 5)
#define CEA_BLOCK_LENGTH(b)       ((b) & 0x1F)
#define CEA_BLOCK_VENDOR          3

//
// IEEE OUI of HDMI Licensing, LLC, as stored in a vendor specific data block.
//
#define HDMI_OUI_0  0x03
#define HDMI_OUI_1  0x0C
#define HDMI_OUI_2  0x00

/**
  Logs the monitor name and the preferred (first detailed) timing of an EDID
  base block.

  @param[in]  Edid  The base block.
**/
STATIC
VOID
EdidLogBaseBlock (
  IN CONST UINT8  *Edid
  )
{
  UINT16       Manufacturer;
  CHAR8        Name[14];
  CONST UINT8  *Descriptor;
  UINTN        Index;
  UINTN        Length;
  UINT32       PixelClockKhz;

  Manufacturer = (UINT16)((Edid[EDID_MANUFACTURER] << 8) | Edid[EDID_MANUFACTURER + 1]);

  Name[0] = '\0';
  for (Index = 0; Index < EDID_DESCRIPTOR_COUNT; Index++) {
    Descriptor = &Edid[EDID_DESCRIPTOR (Index)];
    if ((Descriptor[0] == 0) && (Descriptor[1] == 0) && (Descriptor[3] == EDID_DESCRIPTOR_NAME)) {
      for (Length = 0; Length < sizeof (Name) - 1 && Descriptor[5 + Length] != '\n'; Length++) {
        Name[Length] = (CHAR8)Descriptor[5 + Length];
      }

      Name[Length] = '\0';
      break;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: EDID %u.%u, %c%c%c %04x \"%a\"\n",
    __func__,
    Edid[EDID_VERSION],
    Edid[EDID_REVISION],
    '@' + ((Manufacturer >> 10) & 0x1F),
    '@' + ((Manufacturer >> 5) & 0x1F),
    '@' + (Manufacturer & 0x1F),
    Edid[EDID_PRODUCT] | (Edid[EDID_PRODUCT + 1] << 8),
    Name
    ));

  Descriptor    = &Edid[EDID_DESCRIPTOR (0)];
  PixelClockKhz = (Descriptor[0] | (Descriptor[1] << 8)) * 10;
  if (PixelClockKhz != 0) {
    DEBUG ((
      DEBUG_INFO,
      "%a: preferred timing %ux%u, %u kHz\n",
      __func__,
      Descriptor[2] | ((Descriptor[4] & 0xF0) << 4),
      Descriptor[5] | ((Descriptor[7] & 0xF0) << 4),
      PixelClockKhz
      ));
  }
}

/**
  Returns whether a CEA-861 extension block has an HDMI vendor specific data
  block.

  @param[in]  Block  The extension block.

  @return  TRUE if it does.
**/
STATIC
BOOLEAN
CeaHasHdmiBlock (
  IN CONST UINT8  *Block
  )
{
  UINTN  Offset;
  UINTN  End;
  UINTN  Length;

  End = Block[CEA_DTD_OFFSET];
  if ((End < CEA_DATA_BLOCKS) || (End > EDID_BLOCK_SIZE - 1)) {
    End = EDID_BLOCK_SIZE - 1;
  }

  for (Offset = CEA_DATA_BLOCKS; Offset < End; Offset += 1 + Length) {
    Length = CEA_BLOCK_LENGTH (Block[Offset]);
    if ((CEA_BLOCK_TAG (Block[Offset]) == CEA_BLOCK_VENDOR) &&
        (Length >= 3) && (Offset + 3 < End) &&
        (Block[Offset + 1] == HDMI_OUI_0) &&
        (Block[Offset + 2] == HDMI_OUI_1) &&
        (Block[Offset + 3] == HDMI_OUI_2))
    {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Logs an EDID and returns whether the sink is HDMI (has an HDMI vendor
  specific data block) rather than DVI.

  @param[in]  Edid      The EDID.
  @param[in]  EdidSize  Its size.

  @return  TRUE for an HDMI sink.
**/
BOOLEAN
EdidParse (
  IN CONST UINT8  *Edid,
  IN UINTN        EdidSize
  )
{
  BOOLEAN  Hdmi;

  if (EdidSize < EDID_BLOCK_SIZE) {
    return TRUE;
  }

  EdidLogBaseBlock (Edid);

  if (Edid[EDID_EXTENSION_COUNT] == 0) {
    Hdmi = FALSE;
  } else if (EdidSize < 2 * EDID_BLOCK_SIZE) {
    //
    // An extension we have not read; HDMI sinks have one, so assume HDMI.
    //
    Hdmi = TRUE;
  } else {
    Hdmi = (Edid[EDID_BLOCK_SIZE] == CEA_EXTENSION_TAG) && CeaHasHdmiBlock (&Edid[EDID_BLOCK_SIZE]);
  }

  DEBUG ((DEBUG_INFO, "%a: %a sink\n", __func__, Hdmi ? "HDMI" : "DVI"));

  return Hdmi;
}
