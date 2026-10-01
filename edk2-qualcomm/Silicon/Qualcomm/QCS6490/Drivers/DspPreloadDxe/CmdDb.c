/** @file
  Lookups in the AOP command DB, which maps RPMh resource names to their
  addresses and, for power rails, to their table of levels (Linux's
  drivers/soc/qcom/cmd-db.c).

  The command DB is a header with up to eight resource groups (one per
  accelerator type), each pointing at an array of 24-byte entries and at the
  auxiliary data of those entries, both relative to the end of the header.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "DspPreload.h"

#define CMD_DB_MAX_SLV_ID    8
#define CMD_DB_ID_LENGTH     8

//
// A power rail has at most this many levels (rpmhpd RPMH_ARC_MAX_LEVELS).
//
#define CMD_DB_ARC_MAX_LEVELS  16

#pragma pack(1)

typedef struct {
  UINT8     Id[CMD_DB_ID_LENGTH];
  UINT32    Priority[2];
  UINT32    Addr;
  UINT16    Len;
  UINT16    Offset;
} CMD_DB_ENTRY;

typedef struct {
  UINT16    SlvId;
  UINT16    HeaderOffset;
  UINT16    DataOffset;
  UINT16    Cnt;
  UINT16    Version;
  UINT16    Reserved[3];
} CMD_DB_RSC_HDR;

typedef struct {
  UINT32            Version;
  UINT8             Magic[4];
  CMD_DB_RSC_HDR    Header[CMD_DB_MAX_SLV_ID];
  UINT32            Checksum;
  UINT32            Reserved;
} CMD_DB_HEADER;

#pragma pack()

STATIC CONST UINT8  mCmdDbMagic[] = { 0xDB, 0x30, 0x03, 0x0C };

STATIC CONST UINT8  *mCmdDb;
STATIC UINT64       mCmdDbSize;

EFI_STATUS
CmdDbInit (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_STATUS  Status;

  if (mCmdDb != NULL) {
    return EFI_SUCCESS;
  }

  if (Size < sizeof (CMD_DB_HEADER)) {
    return EFI_NOT_FOUND;
  }

  Status = DspSetMemoryAttributes (Base, Size, EFI_MEMORY_WC | EFI_MEMORY_XP | EFI_MEMORY_RO);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot map the command DB: %r\n", __func__, Status));
    return Status;
  }

  if (CompareMem (((CONST CMD_DB_HEADER *)(UINTN)Base)->Magic, mCmdDbMagic, sizeof (mCmdDbMagic)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: no command DB at 0x%lx\n", __func__, Base));
    return EFI_NOT_FOUND;
  }

  mCmdDb     = (CONST UINT8 *)(UINTN)Base;
  mCmdDbSize = Size;
  return EFI_SUCCESS;
}

EFI_STATUS
CmdDbLookup (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Addr,
  OUT CONST UINT8  **AuxData OPTIONAL,
  OUT UINTN        *AuxSize OPTIONAL
  )
{
  CONST CMD_DB_HEADER   *Header;
  CONST CMD_DB_RSC_HDR  *Rsc;
  CONST CMD_DB_ENTRY    *Entry;
  CONST UINT8           *Data;
  UINT8                 Query[CMD_DB_ID_LENGTH];
  UINTN                 Group;
  UINTN                 Index;
  UINT64                AuxOffset;

  if (mCmdDb == NULL) {
    return EFI_NOT_FOUND;
  }

  ZeroMem (Query, sizeof (Query));
  CopyMem (Query, Name, MIN (AsciiStrLen (Name), sizeof (Query)));

  Header = (CONST CMD_DB_HEADER *)mCmdDb;
  Data   = mCmdDb + sizeof (CMD_DB_HEADER);

  for (Group = 0; Group < CMD_DB_MAX_SLV_ID; Group++) {
    Rsc = &Header->Header[Group];
    if (Rsc->SlvId == 0) {
      break;
    }

    if (sizeof (CMD_DB_HEADER) + Rsc->HeaderOffset + (UINT64)Rsc->Cnt * sizeof (CMD_DB_ENTRY) > mCmdDbSize) {
      return EFI_NOT_FOUND;
    }

    Entry = (CONST CMD_DB_ENTRY *)(Data + Rsc->HeaderOffset);
    for (Index = 0; Index < Rsc->Cnt; Index++, Entry++) {
      if (CompareMem (Entry->Id, Query, sizeof (Query)) != 0) {
        continue;
      }

      *Addr     = Entry->Addr;
      AuxOffset = sizeof (CMD_DB_HEADER) + (UINT64)Rsc->DataOffset + Entry->Offset;
      if (AuxOffset + Entry->Len > mCmdDbSize) {
        return EFI_NOT_FOUND;
      }

      if (AuxData != NULL) {
        *AuxData = mCmdDb + AuxOffset;
      }

      if (AuxSize != NULL) {
        *AuxSize = Entry->Len;
      }

      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
CmdDbRailMaxLevel (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Addr,
  OUT UINT32       *MaxLevel
  )
{
  EFI_STATUS   Status;
  CONST UINT8  *Aux;
  UINTN        AuxSize;
  UINTN        Count;
  UINTN        Index;
  UINT16       Level;

  Status = CmdDbLookup (Name, Addr, &Aux, &AuxSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // One 16-bit level per hardware level, possibly padded with zeros: the
  // table ends at the first zero after the first entry
  // (rpmhpd_update_level_mapping). Votes are hardware level indices.
  //
  Count = MIN (AuxSize / sizeof (UINT16), CMD_DB_ARC_MAX_LEVELS);
  for (Index = 1; Index < Count; Index++) {
    Level = (UINT16)(Aux[2 * Index] | (Aux[2 * Index + 1] << 8));
    if (Level == 0) {
      break;
    }
  }

  if (Index < 2) {
    return EFI_NOT_FOUND;
  }

  *MaxLevel = (UINT32)(Index - 1);
  return EFI_SUCCESS;
}
