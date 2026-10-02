/** @file
  Reads the exception level the boot firmware asks for from xbl_config.

  xbl_config carries a device tree of settings for the boot chain. Its
  /sw/uefi/uefiplat node has OsConfigTableSelection, 1 for Gunyah and 2 for
  KVM, which is what sets xbl_config.elf and xbl_config_kvm.elf apart; the
  stock Qualcomm UEFI stays at EL1 or leaves Gunyah for EL2 accordingly.
  XBL leaves the address and size of that device tree in a cookie in shared
  IMEM (as found in the stock SEC).

  This runs with the MMU off, so the device tree is walked with aligned
  32-bit big-endian reads and byte reads only (libfdt reads unaligned), and
  every offset is checked against the sizes in its header.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>

#include <Qcs6490NvStore.h>

#include "Qcs6490Early.h"
#include "Qcs6490LibInternal.h"

//
// The shared IMEM cookie XBL fills in: the device tree address (UINT64,
// read as two aligned halves) and its size (UINT32).
//
#define SHARED_IMEM_COOKIE_BASE        0x146AA000
#define XBL_CONFIG_DT_ADDRESS_LOW      (SHARED_IMEM_COOKIE_BASE + 0x58)
#define XBL_CONFIG_DT_ADDRESS_HIGH     (SHARED_IMEM_COOKIE_BASE + 0x5C)
#define XBL_CONFIG_DT_SIZE             (SHARED_IMEM_COOKIE_BASE + 0x60)

//
// Where the device tree may be: in DRAM, 0x80000000-0x280000000 (the 8 GiB
// the largest QCS6490 boards have, from the start of DRAM), or in system
// IMEM, 0x14680000-0x14A00000, which holds the cookie itself.
//
#define XBL_CONFIG_DRAM_BASE  0x80000000ULL
#define XBL_CONFIG_DRAM_END   0x280000000ULL
#define XBL_CONFIG_IMEM_BASE  0x14680000ULL
#define XBL_CONFIG_IMEM_END   0x14A00000ULL

//
// The largest device tree accepted; xbl_config's is a few tens of KiB.
//
#define XBL_CONFIG_DT_MAX_SIZE  SIZE_2MB

//
// Flattened device tree header fields (byte offsets) and structure tokens.
//
#define FDT_MAGIC                  0xD00DFEED
#define FDT_HEADER_MAGIC           0
#define FDT_HEADER_TOTALSIZE       4
#define FDT_HEADER_OFF_DT_STRUCT   8
#define FDT_HEADER_OFF_DT_STRINGS  12
#define FDT_HEADER_VERSION         20
#define FDT_HEADER_SIZE_DT_STRINGS 32
#define FDT_HEADER_SIZE_DT_STRUCT  36
#define FDT_HEADER_SIZE            40

#define FDT_BEGIN_NODE  0x1
#define FDT_END_NODE    0x2
#define FDT_PROP        0x3
#define FDT_NOP         0x4
#define FDT_END         0x9

//
// The node, as its path components from the root, and the property.
//
STATIC CONST CHAR8  mUefiPlatPath[][9] = { "", "sw", "uefi", "uefiplat" };

#define XBL_CONFIG_PROPERTY  "OsConfigTableSelection"

/**
  Reads a big-endian 32-bit value.

  @param[in]  Address  Its address, 4-byte aligned.

  @return  The value.
**/
STATIC
UINT32
ReadBe32 (
  IN UINTN  Address
  )
{
  return SwapBytes32 (MmioRead32 (Address));
}

/**
  Finds the length of a NUL-terminated string that must end before a limit.

  @param[in]  Address  The string.
  @param[in]  Limit    The first address past the space it may take.
  @param[out] Length   Its length, without the NUL.

  @retval TRUE   The string ends before Limit.
  @retval FALSE  It does not.
**/
STATIC
BOOLEAN
GetStringLength (
  IN  UINTN  Address,
  IN  UINTN  Limit,
  OUT UINTN  *Length
  )
{
  for (*Length = 0; Address + *Length < Limit; (*Length)++) {
    if (MmioRead8 (Address + *Length) == '\0') {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Compares a string in the device tree with an expected one.

  @param[in]  Address     The string in the device tree.
  @param[in]  Length      Its length, without the NUL.
  @param[in]  Expected    The expected string.
  @param[in]  AllowUnit   Whether a unit address ("@...") may follow, as for
                          node names.

  @retval TRUE   They match.
  @retval FALSE  They do not.
**/
STATIC
BOOLEAN
StringMatches (
  IN UINTN        Address,
  IN UINTN        Length,
  IN CONST CHAR8  *Expected,
  IN BOOLEAN      AllowUnit
  )
{
  UINTN  Index;

  for (Index = 0; Expected[Index] != '\0'; Index++) {
    if ((Index >= Length) || (MmioRead8 (Address + Index) != (UINT8)Expected[Index])) {
      return FALSE;
    }
  }

  return (Index == Length) || (AllowUnit && (MmioRead8 (Address + Index) == '@'));
}

/**
  Checks that a range lies in DRAM or system IMEM.

  @param[in]  Base  The start of the range.
  @param[in]  Size  Its size.

  @retval TRUE   It does.
  @retval FALSE  It does not.
**/
STATIC
BOOLEAN
IsDeviceTreeRange (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  if ((Base >= XBL_CONFIG_DRAM_BASE) && (Base < XBL_CONFIG_DRAM_END) &&
      (Size <= XBL_CONFIG_DRAM_END - Base))
  {
    return TRUE;
  }

  return (Base >= XBL_CONFIG_IMEM_BASE) && (Base < XBL_CONFIG_IMEM_END) &&
         (Size <= XBL_CONFIG_IMEM_END - Base);
}

/**
  Looks up /sw/uefi/uefiplat OsConfigTableSelection in a device tree whose
  header has been checked.

  @param[in]  Structs      The structure block.
  @param[in]  StructsSize  Its size.
  @param[in]  Strings      The strings block.
  @param[in]  StringsSize  Its size.
  @param[out] Value        The property, if found.

  @retval RETURN_SUCCESS         The property was found.
  @retval RETURN_NOT_FOUND       It is not in the tree.
  @retval RETURN_VOLUME_CORRUPTED  The structure block is not valid.
**/
STATIC
RETURN_STATUS
FindOsConfig (
  IN  UINTN   Structs,
  IN  UINTN   StructsSize,
  IN  UINTN   Strings,
  IN  UINTN   StringsSize,
  OUT UINT32  *Value
  )
{
  UINTN   Offset;
  UINTN   End;
  UINTN   Depth;
  UINTN   Matched;
  UINTN   Length;
  UINTN   NameLength;
  UINT32  Token;
  UINT32  NameOffset;

  Offset  = Structs;
  End     = Structs + StructsSize;
  Depth   = 0;
  Matched = 0;

  //
  // Every token moves Offset on by 4 bytes at least, so this ends.
  //
  while (End - Offset >= sizeof (UINT32)) {
    Token   = ReadBe32 (Offset);
    Offset += sizeof (UINT32);

    switch (Token) {
      case FDT_BEGIN_NODE:
        if (!GetStringLength (Offset, End, &Length)) {
          return RETURN_VOLUME_CORRUPTED;
        }

        Depth++;
        if ((Depth == Matched + 1) && (Matched < ARRAY_SIZE (mUefiPlatPath)) &&
            StringMatches (Offset, Length, mUefiPlatPath[Matched], TRUE))
        {
          Matched++;
        }

        Offset += ALIGN_VALUE (Length + 1, sizeof (UINT32));
        if (Offset > End) {
          return RETURN_VOLUME_CORRUPTED;
        }

        break;

      case FDT_END_NODE:
        if (Depth == 0) {
          return RETURN_VOLUME_CORRUPTED;
        }

        if (Matched == Depth) {
          Matched--;
        }

        Depth--;
        if (Depth == 0) {
          return RETURN_NOT_FOUND;
        }

        break;

      case FDT_PROP:
        if (End - Offset < 2 * sizeof (UINT32)) {
          return RETURN_VOLUME_CORRUPTED;
        }

        Length     = ReadBe32 (Offset);
        NameOffset = ReadBe32 (Offset + sizeof (UINT32));
        Offset    += 2 * sizeof (UINT32);
        if (Length > End - Offset) {
          return RETURN_VOLUME_CORRUPTED;
        }

        if ((Matched == ARRAY_SIZE (mUefiPlatPath)) && (Depth == Matched) &&
            (NameOffset < StringsSize) &&
            GetStringLength (Strings + NameOffset, Strings + StringsSize, &NameLength) &&
            StringMatches (Strings + NameOffset, NameLength, XBL_CONFIG_PROPERTY, FALSE))
        {
          if (Length != sizeof (UINT32)) {
            return RETURN_VOLUME_CORRUPTED;
          }

          *Value = ReadBe32 (Offset);
          return RETURN_SUCCESS;
        }

        Offset += ALIGN_VALUE (Length, sizeof (UINT32));
        if (Offset > End) {
          return RETURN_VOLUME_CORRUPTED;
        }

        break;

      case FDT_NOP:
        break;

      case FDT_END:
        return RETURN_NOT_FOUND;

      default:
        return RETURN_VOLUME_CORRUPTED;
    }
  }

  return RETURN_VOLUME_CORRUPTED;
}

/**
  Reads OsConfigTableSelection from xbl_config's device tree.

  @return  QCS6490_XBL_OS_CONFIG_GUNYAH or _KVM, or
           QCS6490_XBL_OS_CONFIG_UNKNOWN.
**/
UINT8
Qcs6490EarlyReadXblConfig (
  VOID
  )
{
  UINT64         Base;
  UINT32         Size;
  UINT32         TotalSize;
  UINT32         StructsOffset;
  UINT32         StringsOffset;
  UINT32         StructsSize;
  UINT32         StringsSize;
  UINT32         Value;
  RETURN_STATUS  Status;

  Base = LShiftU64 (MmioRead32 (XBL_CONFIG_DT_ADDRESS_HIGH), 32) |
         MmioRead32 (XBL_CONFIG_DT_ADDRESS_LOW);
  Size = MmioRead32 (XBL_CONFIG_DT_SIZE);

  if ((Base == 0) || ((Base & (sizeof (UINT32) - 1)) != 0) ||
      (Size < FDT_HEADER_SIZE) || (Size > XBL_CONFIG_DT_MAX_SIZE) ||
      !IsDeviceTreeRange (Base, Size))
  {
    Qcs6490Print ("QCS6490: xbl_config: no device tree (0x%lx, 0x%x bytes)\n", Base, Size);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  if (ReadBe32 ((UINTN)Base + FDT_HEADER_MAGIC) != FDT_MAGIC) {
    Qcs6490Print ("QCS6490: xbl_config: no device tree at 0x%lx\n", Base);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  TotalSize     = ReadBe32 ((UINTN)Base + FDT_HEADER_TOTALSIZE);
  StructsOffset = ReadBe32 ((UINTN)Base + FDT_HEADER_OFF_DT_STRUCT);
  StringsOffset = ReadBe32 ((UINTN)Base + FDT_HEADER_OFF_DT_STRINGS);
  if ((TotalSize < FDT_HEADER_SIZE) || (TotalSize > Size) ||
      ((StructsOffset % sizeof (UINT32)) != 0) ||
      (StructsOffset < FDT_HEADER_SIZE) || (StructsOffset >= TotalSize) ||
      (StringsOffset < FDT_HEADER_SIZE) || (StringsOffset > TotalSize))
  {
    Qcs6490Print ("QCS6490: xbl_config: device tree at 0x%lx is not valid\n", Base);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  //
  // Version 17 gives the block sizes; before, the blocks run to the end.
  //
  StructsSize = TotalSize - StructsOffset;
  StringsSize = TotalSize - StringsOffset;
  if (ReadBe32 ((UINTN)Base + FDT_HEADER_VERSION) >= 17) {
    if ((ReadBe32 ((UINTN)Base + FDT_HEADER_SIZE_DT_STRUCT) > StructsSize) ||
        (ReadBe32 ((UINTN)Base + FDT_HEADER_SIZE_DT_STRINGS) > StringsSize))
    {
      Qcs6490Print ("QCS6490: xbl_config: device tree at 0x%lx is not valid\n", Base);
      return QCS6490_XBL_OS_CONFIG_UNKNOWN;
    }

    StructsSize = ReadBe32 ((UINTN)Base + FDT_HEADER_SIZE_DT_STRUCT);
    StringsSize = ReadBe32 ((UINTN)Base + FDT_HEADER_SIZE_DT_STRINGS);
  }

  Status = FindOsConfig (
             (UINTN)Base + StructsOffset,
             StructsSize,
             (UINTN)Base + StringsOffset,
             StringsSize,
             &Value
             );
  if (Status == RETURN_NOT_FOUND) {
    Qcs6490Print ("QCS6490: xbl_config: no OsConfigTableSelection (device tree at 0x%lx)\n", Base);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  if (RETURN_ERROR (Status)) {
    Qcs6490Print ("QCS6490: xbl_config: device tree at 0x%lx is not valid\n", Base);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  if ((Value != QCS6490_XBL_OS_CONFIG_GUNYAH) && (Value != QCS6490_XBL_OS_CONFIG_KVM)) {
    Qcs6490Print ("QCS6490: xbl_config: OsConfigTableSelection %u not known\n", Value);
    return QCS6490_XBL_OS_CONFIG_UNKNOWN;
  }

  Qcs6490Print (
    "QCS6490: xbl_config: OsConfigTableSelection %u (%a), device tree at 0x%lx\n",
    Value,
    (Value == QCS6490_XBL_OS_CONFIG_KVM) ? "KVM" : "Gunyah",
    Base
    );

  return (UINT8)Value;
}
