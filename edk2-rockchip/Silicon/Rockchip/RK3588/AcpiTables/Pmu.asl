/** @file
 *
 *  RK3588 PMU power domain control.
 *
 *  Devices that sit in a switchable power domain (GPU, video codecs, ...)
 *  call PDON/PDOF from their _PS0/_PS3 methods. The sequence follows the
 *  Linux rockchip pm-domains driver and TF-A's pmu_set_power_domain():
 *
 *    on:  power switch on -> (memory reset) -> wait on -> leave bus idle
 *    off: request bus idle -> power switch off -> wait off
 *
 *  Each domain is described by a package (see the end of this file):
 *    { PwrOffset, PwrMask, MemOffset, MemStatusMask, RepairMask,
 *      ReqOffset, ReqMask, IdleMask }
 *  Offsets are relative to the base of each register group, masks select
 *  the domain's bit. A zero ReqMask means the domain has no bus idle
 *  request, a zero RepairMask means the power status register is used
 *  instead of the repair status, and a zero MemStatusMask skips the memory
 *  reset. IdleMask applies to both the idle acknowledge and idle status
 *  registers.
 *
 *  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiTables.h"

#define PMU2_BASE                 0xFD8D8000
#define PMU2_SIZE                 0x400

#define PMU2_BUS_IDLE_SFTCON      0x10C
#define PMU2_BUS_IDLE_ACK         0x118
#define PMU2_BUS_IDLE_ST          0x120
#define PMU2_PWR_GATE_SFTCON      0x14C
#define PMU2_PWR_GATE_ST          0x180
#define PMU2_MEMPWR_GATE_SFTCON   0x1A0
#define PMU2_PWR_CHAIN1_ST        0x1F0
#define PMU2_PWR_MEM_ST           0x1F8
#define PMU2_BISR_STATUS4         0x290

// Poll budget for every wait, in microseconds.
#define PMU_POLL_TIMEOUT_US       10000

Device (PMU2) {
  Name (_HID, "PNP0C02")
  Name (_UID, 0x3588)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, PMU2_BASE, PMU2_SIZE)
  })

  OperationRegion (PMUR, SystemMemory, PMU2_BASE, PMU2_SIZE)
  Field (PMUR, DWordAcc, NoLock, Preserve) {
    Offset (PMU2_BUS_IDLE_SFTCON),
    BIS0, 32,
    BIS1, 32,
    BIS2, 32,
    Offset (PMU2_BUS_IDLE_ACK),
    BIA0, 32,
    BIA1, 32,
    IDS0, 32,   // PMU2_BUS_IDLE_ST
    IDS1, 32,
    Offset (PMU2_PWR_GATE_SFTCON),
    PGS0, 32,
    PGS1, 32,
    Offset (PMU2_PWR_GATE_ST),
    PGT0, 32,
    PGT1, 32,
    Offset (PMU2_MEMPWR_GATE_SFTCON),
    MGS0, 32,
    MGS1, 32,
    Offset (PMU2_PWR_CHAIN1_ST),
    PCH0, 32,
    PCH1, 32,
    PMT0, 32,   // PMU2_PWR_MEM_ST
    PMT1, 32,
    Offset (PMU2_BISR_STATUS4),
    BISR, 32
  }

  //
  // Register accessors indexed by the offsets used in the domain packages.
  // Only the offsets that appear in the RK3588 domain table are handled.
  //
  Method (RDRG, 1, Serialized) {
    Switch (ToInteger (Arg0)) {
      Case (0x118) { Return (BIA0) }
      Case (0x11C) { Return (BIA1) }
      Case (0x120) { Return (IDS0) }
      Case (0x124) { Return (IDS1) }
      Case (0x180) { Return (PGT0) }
      Case (0x184) { Return (PGT1) }
      Case (0x1F0) { Return (PCH0) }
      Case (0x1F4) { Return (PCH1) }
      Case (0x1F8) { Return (PMT0) }
      Case (0x1FC) { Return (PMT1) }
      Case (0x290) { Return (BISR) }
    }
    Return (0)
  }

  // All writable registers here use the upper 16 bits as a write enable mask.
  Method (WRRG, 2, Serialized) {
    Switch (ToInteger (Arg0)) {
      Case (0x10C) { BIS0 = Arg1 }
      Case (0x110) { BIS1 = Arg1 }
      Case (0x114) { BIS2 = Arg1 }
      Case (0x14C) { PGS0 = Arg1 }
      Case (0x150) { PGS1 = Arg1 }
      Case (0x1A0) { MGS0 = Arg1 }
      Case (0x1A4) { MGS1 = Arg1 }
    }
  }

  //
  // Wait until (register & mask) == value.
  //   Arg0: register offset, Arg1: mask, Arg2: expected value
  // Returns 1 on success, 0 on timeout.
  //
  Method (PWAT, 3, Serialized) {
    For (Local0 = 0, Local0 < PMU_POLL_TIMEOUT_US, Local0++) {
      If ((RDRG (Arg0) & Arg1) == Arg2) {
        Return (1)
      }
      Stall (1)
    }
    Return (0)
  }

  //
  // PDST - Power Domain Status
  //   Arg0: domain package
  // Returns 1 if the domain is on, 0 if it is off. Callers compare the
  // result against 1, so this must not return Ones like the logical
  // operators do.
  //
  Method (PDST, 1, Serialized) {
    Local0 = DerefOf (Arg0[4])  // RepairMask
    If (Local0 != 0) {
      // Repair status: bit set means powered on.
      If (BISR & Local0) {
        Return (1)
      }
      Return (0)
    }

    // Power gate status: bit set means powered off.
    Local1 = DerefOf (Arg0[1])  // PwrMask
    If (RDRG (PMU2_PWR_GATE_ST + DerefOf (Arg0[0])) & Local1) {
      Return (0)
    }
    Return (1)
  }

  //
  // IDLE - Request the domain's bus interface to enter or leave idle.
  //   Arg0: domain package, Arg1: 1 to enter idle, 0 to leave it
  // Returns 1 on success, 0 on timeout.
  //
  Method (IDLE, 2, Serialized) {
    Local0 = DerefOf (Arg0[6])  // ReqMask
    If (Local0 == 0) {
      Return (1)
    }
    Local1 = DerefOf (Arg0[5])  // ReqOffset
    Local2 = DerefOf (Arg0[7])  // IdleMask

    If (Arg1) {
      WRRG (PMU2_BUS_IDLE_SFTCON + Local1, (Local0 << 16) | Local0)
      Local3 = Local2
    } Else {
      WRRG (PMU2_BUS_IDLE_SFTCON + Local1, Local0 << 16)
      Local3 = 0
    }

    If (PWAT (PMU2_BUS_IDLE_ACK, Local2, Local3) == 0) {
      Return (0)
    }
    Return (PWAT (PMU2_BUS_IDLE_ST, Local2, Local3))
  }

  //
  // MRST - Reset the domain's memories after powering it back on while
  // they were still powered.
  //   Arg0: domain package
  // Returns 1 on success, 0 on timeout.
  //
  Method (MRST, 1, Serialized) {
    Local0 = DerefOf (Arg0[0])  // PwrOffset
    Local1 = DerefOf (Arg0[1])  // PwrMask
    Local2 = DerefOf (Arg0[2])  // MemOffset
    Local3 = DerefOf (Arg0[3])  // MemStatusMask

    // Wait for the power chain to come up.
    If (PWAT (PMU2_PWR_CHAIN1_ST + Local2, Local3, Local3) == 0) {
      Return (0)
    }
    Stall (20)

    // Memory off (status bit set means off)...
    WRRG (PMU2_MEMPWR_GATE_SFTCON + Local0, (Local1 << 16) | Local1)
    If (PWAT (PMU2_PWR_MEM_ST + Local2, Local3, Local3) == 0) {
      Return (0)
    }

    // ...and back on.
    WRRG (PMU2_MEMPWR_GATE_SFTCON + Local0, Local1 << 16)
    Return (PWAT (PMU2_PWR_MEM_ST + Local2, Local3, 0))
  }

  //
  // PDSW - Switch the domain's power.
  //   Arg0: domain package, Arg1: 1 for on, 0 for off
  // Returns 1 on success, 0 on timeout.
  //
  Method (PDSW, 2, Serialized) {
    Local0 = DerefOf (Arg0[0])  // PwrOffset
    Local1 = DerefOf (Arg0[1])  // PwrMask
    Local2 = DerefOf (Arg0[2])  // MemOffset
    Local3 = DerefOf (Arg0[3])  // MemStatusMask
    Local4 = 0                  // Memories still powered

    If (Arg1 && (Local3 != 0)) {
      Local4 = ((RDRG (PMU2_PWR_MEM_ST + Local2) & Local3) == 0)
    }

    // Bit set means the domain is switched off.
    If (Arg1) {
      WRRG (PMU2_PWR_GATE_SFTCON + Local0, Local1 << 16)
    } Else {
      WRRG (PMU2_PWR_GATE_SFTCON + Local0, (Local1 << 16) | Local1)
    }

    If (Local4) {
      If (MRST (Arg0) == 0) {
        Return (0)
      }
    }

    For (Local5 = 0, Local5 < PMU_POLL_TIMEOUT_US, Local5++) {
      If (PDST (Arg0) == Arg1) {
        Return (1)
      }
      Stall (1)
    }
    Return (0)
  }

  //
  // PDON - Power on a domain and let its bus leave idle.
  //   Arg0: domain package
  // Returns 1 on success, 0 on failure.
  //
  Method (PDON, 1, Serialized) {
    If (PDST (Arg0)) {
      Return (1)
    }
    If (PDSW (Arg0, 1) == 0) {
      Return (0)
    }
    Return (IDLE (Arg0, 0))
  }

  //
  // PDOF - Idle a domain's bus and power the domain off.
  //   Arg0: domain package
  // Returns 1 on success, 0 on failure.
  //
  Method (PDOF, 1, Serialized) {
    If (PDST (Arg0) == 0) {
      Return (1)
    }
    If (IDLE (Arg0, 1) == 0) {
      Return (0)
    }
    Return (PDSW (Arg0, 0))
  }

  //
  // Domain descriptions, taken from the Linux rk3588 power domain table:
  //   { PwrOffset, PwrMask, MemOffset, MemStatusMask, RepairMask,
  //     ReqOffset, ReqMask, IdleMask }
  //
  Name (PGPU, Package () { 0x0, 0x0001, 0x0, 0x00000000, 0x00000002, 0x0, 0x0001, 0x0001 })
  Name (PVE0, Package () { 0x0, 0x0040, 0x0, 0x00004000, 0x00000020, 0x0, 0x0010, 0x0010 })
  Name (PVE1, Package () { 0x0, 0x0080, 0x0, 0x00008000, 0x00000040, 0x0, 0x0020, 0x0020 })
  Name (PVD0, Package () { 0x0, 0x0100, 0x0, 0x00010000, 0x00000080, 0x0, 0x0040, 0x0040 })
  Name (PVD1, Package () { 0x0, 0x0200, 0x0, 0x00020000, 0x00000100, 0x0, 0x0080, 0x0080 })
  Name (PVDP, Package () { 0x0, 0x0400, 0x0, 0x00040000, 0x00000200, 0x0, 0x0100, 0x0100 })
  Name (PR30, Package () { 0x0, 0x0800, 0x0, 0x00080000, 0x00000400, 0x0, 0x0000, 0x0000 })
  Name (PAV1, Package () { 0x0, 0x1000, 0x0, 0x00100000, 0x00000800, 0x0, 0x0200, 0x0200 })
  Name (PR31, Package () { 0x4, 0x0001, 0x0, 0x01000000, 0x00008000, 0x0, 0x1000, 0x1000 })
}
