/** @file
  PCI Segment Library for PCIe0 of the QCS6490.

  Segment 0 has two buses. The root port, 00:00.0, is the controller's own
  configuration space, the DBI (pcie-designware-host.c
  dw_pcie_own_conf_map_bus). The one device below it is on bus 1 and is
  reached the way Linux reaches it (dw_pcie_other_conf_map_bus): outbound
  iATU region 0 turns accesses to the "config" window into CFG0 requests
  for the target bus, device and function. The region is reprogrammed only
  when it does not already point at the target; the region's own
  registers are the record of what it points at, so the record also holds
  after the controller has been reset.

  Nothing is accessed while the controller is powered down or held in
  reset, nothing past the DBI (the ELBI follows it), and nothing below the
  root port while the link is down, where a configuration request can raise
  an SError: such reads return all ones and such writes are dropped, as for
  an absent device. On bus 1 only device 0 is decoded: a type 0
  configuration request reaches the only device on the link whatever the
  device number, so the device would show up 32 times.

  Copyright (c) 2023-2025, Mario Bălănică <mariobalanica02@gmail.com>
  Copyright (c) 2023, Molly Sophia <mollysophia379@gmail.com>
  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
  Copyright (c) 2007 - 2012, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2017, Linaro, Ltd. All rights reserved.<BR>
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/TimerLib.h>

#include <Qcs6490Pcie.h>

typedef enum {
  PciCfgWidthUint8 = 0,
  PciCfgWidthUint16,
  PciCfgWidthUint32,
  PciCfgWidthMax
} PCI_CFG_WIDTH;

/**
  Assert the validity of a PCI Segment address.
  A valid PCI Segment address should not contain 1's in bits 28..31 and 48..63

  @param  A The address to validate.
  @param  M Additional bits to assert to be zero.

**/
#define ASSERT_INVALID_PCI_SEGMENT_ADDRESS(A, M) \
  ASSERT (((A) & (0xffff0000f0000000ULL | (M))) == 0)

//
// What a read of an absent function returns.
//
#define INVALID_PCI_DATA  0xFFFFFFFF

#define GET_SEG_NUM(Address)   ((UINT32)RShiftU64 ((Address), 32) & 0xFFFF)
#define GET_BUS_NUM(Address)   ((UINT32)RShiftU64 ((Address), 20) & 0xFF)
#define GET_DEV_NUM(Address)   ((UINT32)RShiftU64 ((Address), 15) & 0x1F)
#define GET_FUNC_NUM(Address)  ((UINT32)RShiftU64 ((Address), 12) & 0x07)
#define GET_REG_NUM(Address)   ((UINT32)(Address) & 0xFFF)

//
// Size of the window region 0 maps: one function's configuration space
// (the iATU's regions are aligned to 4 KiB on this core).
//
#define CFG_WINDOW_SIZE  SIZE_4KB

/**
  Points outbound iATU region 0 at a function on the bus below the root port,
  unless it already does, programming it the way dw_pcie_prog_outbound_atu
  does: bases, limits, targets, then the type and the enable, and waits for
  the enable to read back. The window is the first 4 KiB of the "config"
  window, sending CFG0 requests: bus 1 is the root port's secondary bus.

  @param[in]  Target  DW_ATU_TARGET_BDF() of the function.

  @retval TRUE   The region points at the function.
  @retval FALSE  The region did not enable.
**/
STATIC
BOOLEAN
PciSegmentLibSetCfgTarget (
  IN UINT32  Target
  )
{
  UINTN  Region;
  UINTN  Retry;

  Region = DW_ATU_OB (DW_ATU_REGION_CFG);

  if (((MmioRead32 (Region + DW_ATU_CTRL2) & DW_ATU_ENABLE) != 0) &&
      ((MmioRead32 (Region + DW_ATU_CTRL1) & 0x1F) == DW_ATU_TYPE_CFG0) &&
      (MmioRead32 (Region + DW_ATU_LWR_BASE) == PCIE0_CFG_BASE) &&
      (MmioRead32 (Region + DW_ATU_LWR_TARGET) == Target))
  {
    return TRUE;
  }

  MmioWrite32 (Region + DW_ATU_LWR_BASE, PCIE0_CFG_BASE);
  MmioWrite32 (Region + DW_ATU_UPR_BASE, 0);
  MmioWrite32 (Region + DW_ATU_LIMIT, PCIE0_CFG_BASE + CFG_WINDOW_SIZE - 1);
  MmioWrite32 (Region + DW_ATU_UPR_LIMIT, 0);
  MmioWrite32 (Region + DW_ATU_LWR_TARGET, Target);
  MmioWrite32 (Region + DW_ATU_UPR_TARGET, 0);
  MmioWrite32 (Region + DW_ATU_CTRL1, DW_ATU_TYPE_CFG0);
  MmioWrite32 (Region + DW_ATU_CTRL2, DW_ATU_ENABLE);

  for (Retry = 0; Retry < DW_ATU_ENABLE_RETRIES; Retry++) {
    if ((MmioRead32 (Region + DW_ATU_CTRL2) & DW_ATU_ENABLE) != 0) {
      return TRUE;
    }

    MicroSecondDelay (DW_ATU_ENABLE_RETRY_US);
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: iATU region %u for target 0x%08x did not enable: CTRL1 0x%x CTRL2 0x%x\n",
    __func__,
    DW_ATU_REGION_CFG,
    Target,
    MmioRead32 (Region + DW_ATU_CTRL1),
    MmioRead32 (Region + DW_ATU_CTRL2)
    ));
  return FALSE;
}

/**
  Accesses a register with the width asked for.

  @param[in]  Address  The register.
  @param[in]  Width    The width.
  @param[in]  Write    TRUE to write Data, FALSE to read.
  @param[in]  Data     The value to write.

  @return  The value read, or Data for a write.
**/
STATIC
UINT32
PciSegmentLibMmio (
  IN UINTN          Address,
  IN PCI_CFG_WIDTH  Width,
  IN BOOLEAN        Write,
  IN UINT32         Data
  )
{
  switch (Width) {
    case PciCfgWidthUint8:
      if (Write) {
        return MmioWrite8 (Address, (UINT8)Data);
      }

      return MmioRead8 (Address);

    case PciCfgWidthUint16:
      if (Write) {
        return MmioWrite16 (Address, (UINT16)Data);
      }

      return MmioRead16 (Address);

    case PciCfgWidthUint32:
      if (Write) {
        return MmioWrite32 (Address, Data);
      }

      return MmioRead32 (Address);

    default:
      ASSERT (FALSE);
      return INVALID_PCI_DATA;
  }
}

/**
  Reads or writes a PCI configuration register of segment 0: 00:00.0
  through the DBI, 01:00.x through iATU region 0. Accesses to anything else,
  or that the state of the controller or the link does not allow, read all
  ones and drop writes.

  The iATU region is programmed and used with interrupts off, so that a
  configuration access from an event cannot retarget it in between.

  @param[in]  Address  The address that encodes the PCI Segment, Bus,
                       Device, Function and Register.
  @param[in]  Width    The width of the access.
  @param[in]  Write    TRUE to write Data, FALSE to read.
  @param[in]  Data     The value to write.

  @return  The value read, or Data for a write.
**/
STATIC
UINT32
PciSegmentLibAccess (
  IN UINT64         Address,
  IN PCI_CFG_WIDTH  Width,
  IN BOOLEAN        Write,
  IN UINT32         Data
  )
{
  UINT32   Bus;
  UINT32   Device;
  UINT32   Function;
  UINT32   Register;
  UINT32   Value;
  BOOLEAN  InterruptState;

  Bus      = GET_BUS_NUM (Address);
  Device   = GET_DEV_NUM (Address);
  Function = GET_FUNC_NUM (Address);
  Register = GET_REG_NUM (Address);
  Value    = Write ? Data : INVALID_PCI_DATA;

  if (GET_SEG_NUM (Address) != PCIE0_SEGMENT) {
    return Value;
  }

  if (Bus == PCIE0_ROOT_BUS) {
    if ((Device != 0) || (Function != 0) || (Register > PCIE0_DBI_LAST_REG) ||
        !Qcs6490Pcie0Accessible ())
    {
      return Value;
    }

    return PciSegmentLibMmio (PCIE0_DBI_BASE + Register, Width, Write, Data);
  }

  if ((Bus != PCIE0_SECONDARY_BUS) || (Device != 0)) {
    return Value;
  }

  InterruptState = SaveAndDisableInterrupts ();

  if (Qcs6490Pcie0LinkUp () &&
      PciSegmentLibSetCfgTarget (DW_ATU_TARGET_BDF (Bus, Device, Function)))
  {
    Value = PciSegmentLibMmio (PCIE0_CFG_BASE + Register, Width, Write, Data);
  }

  SetInterruptState (InterruptState);

  return Value;
}

/**
  Internal worker function to read a PCI configuration register.

  @param  Address The address that encodes the PCI Bus, Device, Function and
                  Register.
  @param  Width   The width of data to read

  @return The value read from the PCI configuration register.

**/
STATIC
UINT32
PciSegmentLibReadWorker (
  IN  UINT64         Address,
  IN  PCI_CFG_WIDTH  Width
  )
{
  return PciSegmentLibAccess (Address, Width, FALSE, 0);
}

/**
  Internal worker function to writes a PCI configuration register.

  @param  Address The address that encodes the PCI Bus, Device, Function and
                  Register.
  @param  Width   The width of data to write
  @param  Data    The value to write.

  @return The value written to the PCI configuration register.

**/
STATIC
UINT32
PciSegmentLibWriteWorker (
  IN  UINT64         Address,
  IN  PCI_CFG_WIDTH  Width,
  IN  UINT32         Data
  )
{
  return PciSegmentLibAccess (Address, Width, TRUE, Data);
}

/**
  Register a PCI device so PCI configuration registers may be accessed after
  SetVirtualAddressMap().

  If any reserved bits in Address are set, then ASSERT().

  @param  Address The address that encodes the PCI Bus, Device, Function and
                  Register.

  @retval RETURN_SUCCESS           The PCI device was registered for runtime access.
  @retval RETURN_UNSUPPORTED       An attempt was made to call this function
                                   after ExitBootServices().
  @retval RETURN_UNSUPPORTED       The resources required to access the PCI device
                                   at runtime could not be mapped.
  @retval RETURN_OUT_OF_RESOURCES  There are not enough resources available to
                                   complete the registration.

**/
RETURN_STATUS
EFIAPI
PciSegmentRegisterForRuntimeAccess (
  IN UINTN  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);
  return RETURN_UNSUPPORTED;
}

/**
  Reads an 8-bit PCI configuration register.

  Reads and returns the 8-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function,
                    and Register.

  @return The 8-bit PCI configuration register specified by Address.

**/
UINT8
EFIAPI
PciSegmentRead8 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);

  return (UINT8)PciSegmentLibReadWorker (Address, PciCfgWidthUint8);
}

/**
  Writes an 8-bit PCI configuration register.

  Writes the 8-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address     The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  Value       The value to write.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentWrite8 (
  IN UINT64  Address,
  IN UINT8   Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);

  return (UINT8)PciSegmentLibWriteWorker (Address, PciCfgWidthUint8, Value);
}

/**
  Performs a bitwise OR of an 8-bit PCI configuration register with an 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentOr8 (
  IN UINT64  Address,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)(PciSegmentRead8 (Address) | OrData));
}

/**
  Performs a bitwise AND of an 8-bit PCI configuration register with an 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.
  If any reserved bits in Address are set, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentAnd8 (
  IN UINT64  Address,
  IN UINT8   AndData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)(PciSegmentRead8 (Address) & AndData));
}

/**
  Performs a bitwise AND of an 8-bit PCI configuration register with an 8-bit value,
  followed a  bitwise OR with another 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData    The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentAndThenOr8 (
  IN UINT64  Address,
  IN UINT8   AndData,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)((PciSegmentRead8 (Address) & AndData) | OrData));
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in an 8-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   The PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldRead8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead8 (PciSegmentRead8 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  8-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  Value     The new value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldWrite8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   Value
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldWrite8 (PciSegmentRead8 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads a bit field in an 8-bit PCI configuration, performs a bitwise OR, and
  writes the result back to the bit field in the 8-bit port.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 8-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized. Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldOr8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldOr8 (PciSegmentRead8 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in an 8-bit PCI configuration register, performs a bitwise
  AND, and writes the result back to the bit field in the 8-bit register.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise AND between the read result and the value specified by AndData, and
  writes the result to the 8-bit PCI configuration register specified by
  Address. The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are
  serialized. Extra left bits in AndData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldAnd8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   AndData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldAnd8 (PciSegmentRead8 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in an 8-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the
  8-bit port.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 8-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldAndThenOr8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   AndData,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldAndThenOr8 (PciSegmentRead8 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a 16-bit PCI configuration register.

  Reads and returns the 16-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.

  @return The 16-bit PCI configuration register specified by Address.

**/
UINT16
EFIAPI
PciSegmentRead16 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 1);

  return (UINT16)PciSegmentLibReadWorker (Address, PciCfgWidthUint16);
}

/**
  Writes a 16-bit PCI configuration register.

  Writes the 16-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address     The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  Value       The value to write.

  @return The parameter of Value.

**/
UINT16
EFIAPI
PciSegmentWrite16 (
  IN UINT64  Address,
  IN UINT16  Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 1);

  return (UINT16)PciSegmentLibWriteWorker (Address, PciCfgWidthUint16, Value);
}

/**
  Performs a bitwise OR of a 16-bit PCI configuration register with
  a 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 16-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address The address that encodes the PCI Segment, Bus, Device, Function and
                  Register.
  @param  OrData  The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentOr16 (
  IN UINT64  Address,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)(PciSegmentRead16 (Address) | OrData));
}

/**
  Performs a bitwise AND of a 16-bit PCI configuration register with a 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentAnd16 (
  IN UINT64  Address,
  IN UINT16  AndData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)(PciSegmentRead16 (Address) & AndData));
}

/**
  Performs a bitwise AND of a 16-bit PCI configuration register with a 16-bit value,
  followed a  bitwise OR with another 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentAndThenOr16 (
  IN UINT64  Address,
  IN UINT16  AndData,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)((PciSegmentRead16 (Address) & AndData) | OrData));
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in a 16-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   The PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldRead16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead16 (PciSegmentRead16 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  16-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  Value     The new value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldWrite16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  Value
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldWrite16 (PciSegmentRead16 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 16-bit PCI configuration register specified by Address.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldOr16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldOr16 (PciSegmentRead16 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in a 16-bit PCI configuration, performs a bitwise OR,
  and writes the result back to the bit field in the 16-bit port.

  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.
  Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    The ordinal of the least significant bit in a byte is bit 0.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    The ordinal of the most significant bit in a byte is bit 7.
  @param  AndData   The value to AND with the read value from the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldAnd16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  AndData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldAnd16 (PciSegmentRead16 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in a 16-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the
  16-bit port.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 16-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldAndThenOr16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  AndData,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldAndThenOr16 (PciSegmentRead16 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a 32-bit PCI configuration register.

  Reads and returns the 32-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function,
                    and Register.

  @return The 32-bit PCI configuration register specified by Address.

**/
UINT32
EFIAPI
PciSegmentRead32 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 3);

  return PciSegmentLibReadWorker (Address, PciCfgWidthUint32);
}

/**
  Writes a 32-bit PCI configuration register.

  Writes the 32-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address     The address that encodes the PCI Segment, Bus, Device,
                      Function, and Register.
  @param  Value       The value to write.

  @return The parameter of Value.

**/
UINT32
EFIAPI
PciSegmentWrite32 (
  IN UINT64  Address,
  IN UINT32  Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 3);

  return PciSegmentLibWriteWorker (Address, PciCfgWidthUint32, Value);
}

/**
  Performs a bitwise OR of a 32-bit PCI configuration register with a 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentOr32 (
  IN UINT64  Address,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (Address, PciSegmentRead32 (Address) | OrData);
}

/**
  Performs a bitwise AND of a 32-bit PCI configuration register with a 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function,
                    and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentAnd32 (
  IN UINT64  Address,
  IN UINT32  AndData
  )
{
  return PciSegmentWrite32 (Address, PciSegmentRead32 (Address) & AndData);
}

/**
  Performs a bitwise AND of a 32-bit PCI configuration register with a 32-bit value,
  followed a  bitwise OR with another 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   The address that encodes the PCI Segment, Bus, Device, Function,
                    and Register.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentAndThenOr32 (
  IN UINT64  Address,
  IN UINT32  AndData,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (Address, (PciSegmentRead32 (Address) & AndData) | OrData);
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in a 32-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   The PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldRead32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead32 (PciSegmentRead32 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  32-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  Value     The new value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldWrite32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  Value
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldWrite32 (PciSegmentRead32 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads a bit field in a 32-bit PCI configuration, performs a bitwise OR, and
  writes the result back to the bit field in the 32-bit port.

  Reads the 32-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 32-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized. Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldOr32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldOr32 (PciSegmentRead32 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in a 32-bit PCI configuration register, performs a bitwise
  AND, and writes the result back to the bit field in the 32-bit register.


  Reads the 32-bit PCI configuration register specified by Address, performs a bitwise
  AND between the read result and the value specified by AndData, and writes the result
  to the 32-bit PCI configuration register specified by Address. The value written to
  the PCI configuration register is returned.  This function must guarantee that all PCI
  read and write operations are serialized.  Extra left bits in AndData are stripped.
  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldAnd32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  AndData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldAnd32 (PciSegmentRead32 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in a 32-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the
  32-bit port.

  Reads the 32-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 32-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   The PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldAndThenOr32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  AndData,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldAndThenOr32 (PciSegmentRead32 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a range of PCI configuration registers into a caller supplied buffer.

  Reads the range of PCI configuration registers specified by StartAddress and
  Size into the buffer specified by Buffer. This function only allows the PCI
  configuration registers from a single PCI function to be read. Size is
  returned. When possible 32-bit PCI configuration read cycles are used to read
  from StartAdress to StartAddress + Size. Due to alignment restrictions, 8-bit
  and 16-bit PCI configuration read cycles may be used at the beginning and the
  end of the range.

  If any reserved bits in StartAddress are set, then ASSERT().
  If ((StartAddress & 0xFFF) + Size) > 0x1000, then ASSERT().
  If Size > 0 and Buffer is NULL, then ASSERT().

  @param  StartAddress  The starting address that encodes the PCI Segment, Bus,
                        Device, Function and Register.
  @param  Size          The size in bytes of the transfer.
  @param  Buffer        The pointer to a buffer receiving the data read.

  @return Size

**/
UINTN
EFIAPI
PciSegmentReadBuffer (
  IN  UINT64  StartAddress,
  IN  UINTN   Size,
  OUT VOID    *Buffer
  )
{
  UINTN  ReturnValue;

  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (StartAddress, 0);
  ASSERT (((StartAddress & 0xFFF) + Size) <= 0x1000);

  if (Size == 0) {
    return Size;
  }

  ASSERT (Buffer != NULL);

  //
  // Save Size for return
  //
  ReturnValue = Size;

  if ((StartAddress & BIT0) != 0) {
    //
    // Read a byte if StartAddress is byte aligned
    //
    *(volatile UINT8 *)Buffer = PciSegmentRead8 (StartAddress);
    StartAddress             += sizeof (UINT8);
    Size                     -= sizeof (UINT8);
    Buffer                    = (UINT8 *)Buffer + 1;
  }

  if ((Size >= sizeof (UINT16)) && ((StartAddress & BIT1) != 0)) {
    //
    // Read a word if StartAddress is word aligned
    //
    WriteUnaligned16 (Buffer, PciSegmentRead16 (StartAddress));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  while (Size >= sizeof (UINT32)) {
    //
    // Read as many double words as possible
    //
    WriteUnaligned32 (Buffer, PciSegmentRead32 (StartAddress));
    StartAddress += sizeof (UINT32);
    Size         -= sizeof (UINT32);
    Buffer        = (UINT32 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT16)) {
    //
    // Read the last remaining word if exist
    //
    WriteUnaligned16 (Buffer, PciSegmentRead16 (StartAddress));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT8)) {
    //
    // Read the last remaining byte if exist
    //
    *(volatile UINT8 *)Buffer = PciSegmentRead8 (StartAddress);
  }

  return ReturnValue;
}

/**
  Copies the data in a caller supplied buffer to a specified range of PCI
  configuration space.

  Writes the range of PCI configuration registers specified by StartAddress and
  Size from the buffer specified by Buffer. This function only allows the PCI
  configuration registers from a single PCI function to be written. Size is
  returned. When possible 32-bit PCI configuration write cycles are used to
  write from StartAdress to StartAddress + Size. Due to alignment restrictions,
  8-bit and 16-bit PCI configuration write cycles may be used at the beginning
  and the end of the range.

  If any reserved bits in StartAddress are set, then ASSERT().
  If ((StartAddress & 0xFFF) + Size) > 0x1000, then ASSERT().
  If Size > 0 and Buffer is NULL, then ASSERT().

  @param  StartAddress  The starting address that encodes the PCI Segment, Bus,
                        Device, Function and Register.
  @param  Size          The size in bytes of the transfer.
  @param  Buffer        The pointer to a buffer containing the data to write.

  @return The parameter of Size.

**/
UINTN
EFIAPI
PciSegmentWriteBuffer (
  IN UINT64  StartAddress,
  IN UINTN   Size,
  IN VOID    *Buffer
  )
{
  UINTN  ReturnValue;

  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (StartAddress, 0);
  ASSERT (((StartAddress & 0xFFF) + Size) <= 0x1000);

  if (Size == 0) {
    return 0;
  }

  ASSERT (Buffer != NULL);

  //
  // Save Size for return
  //
  ReturnValue = Size;

  if ((StartAddress & BIT0) != 0) {
    //
    // Write a byte if StartAddress is byte aligned
    //
    PciSegmentWrite8 (StartAddress, *(UINT8 *)Buffer);
    StartAddress += sizeof (UINT8);
    Size         -= sizeof (UINT8);
    Buffer        = (UINT8 *)Buffer + 1;
  }

  if ((Size >= sizeof (UINT16)) && ((StartAddress & BIT1) != 0)) {
    //
    // Write a word if StartAddress is word aligned
    //
    PciSegmentWrite16 (StartAddress, ReadUnaligned16 (Buffer));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  while (Size >= sizeof (UINT32)) {
    //
    // Write as many double words as possible
    //
    PciSegmentWrite32 (StartAddress, ReadUnaligned32 (Buffer));
    StartAddress += sizeof (UINT32);
    Size         -= sizeof (UINT32);
    Buffer        = (UINT32 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT16)) {
    //
    // Write the last remaining word if exist
    //
    PciSegmentWrite16 (StartAddress, ReadUnaligned16 (Buffer));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT8)) {
    //
    // Write the last remaining byte if exist
    //
    PciSegmentWrite8 (StartAddress, *(UINT8 *)Buffer);
  }

  return ReturnValue;
}
