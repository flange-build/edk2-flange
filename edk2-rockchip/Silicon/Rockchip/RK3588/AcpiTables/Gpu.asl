/** @file
 *
 *  RK3588 Mali-G610 (Valhall CSF) GPU.
 *
 *  Power is handled through _PS0/_PS3, which switch the GPU power domain.
 *  The GPU core clock is owned by the secure firmware (SCMI clock 5,
 *  backed by the GPU PVTPLL) and is set through _DSM.
 *
 *  The vdd_gpu rail is left at the voltage programmed by the platform
 *  code (750 mV on the reference RK806 design), so the clock is capped
 *  at the highest OPP that voltage supports.
 *
 *  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiTables.h"

#define GPU_DSM_UUID          ToUUID ("7a8c1c86-9a3b-4f33-9c55-33e1b0a2c410")
#define GPU_DSM_REVISION      0

#define GPU_SCMI_CLOCK_ID     5

#define GPU_CLOCK_DEFAULT_HZ  200000000
#ifndef GPU_CLOCK_MAX_HZ
#define GPU_CLOCK_MAX_HZ      800000000   // Highest OPP at 750 mV
#endif

// CRU_GATE_CON66/67: GPU clocks, NIUs and GRF (bit set = gated).
#define CRU_GATE_CON66        0xFD7C0908
#define GPU_GATE_CON66_MASK   0x7FDA
#define GPU_GATE_CON67_MASK   0x0006

Device (GPU0) {
  Name (_HID, "RKCP0E20")
  Name (_UID, 0)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFB000000, 0x200000)
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 124 } // job
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 125 } // mmu
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 126 } // gpu
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", Package () { "rockchip,rk3588-mali", "arm,mali-valhall-csf" } },
      Package () { "interrupt-names", Package () { "job", "mmu", "gpu" } },
      Package () { "max-frequency", GPU_CLOCK_MAX_HZ },
    }
  })

  OperationRegion (CRUG, SystemMemory, CRU_GATE_CON66, 0x8)
  Field (CRUG, DWordAcc, NoLock, WriteAsZeros) {
    GC66, 32,
    GC67, 32
  }

  // Last rate requested through _DSM, restored on the next _PS0.
  Name (GCLK, GPU_CLOCK_DEFAULT_HZ)

  //
  // Set the GPU clock.
  //   Arg0: rate in Hz
  // Returns the rate that was set, or 0 on failure.
  //
  Method (SCLK, 1, Serialized) {
    Local0 = Arg0
    If (Local0 > GPU_CLOCK_MAX_HZ) {
      Local0 = GPU_CLOCK_MAX_HZ
    }

    Local1 = \_SB.SCMI.CLRS (GPU_SCMI_CLOCK_ID, Local0)
    If (DerefOf (Local1[0]) != 0) {
      Return (0)
    }

    GCLK = Local0
    Return (Local0)
  }

  Method (_PS0, 0, Serialized) {
    // Ungate the domain's clocks before it comes up.
    GC66 = GPU_GATE_CON66_MASK << 16
    GC67 = GPU_GATE_CON67_MASK << 16

    \_SB.PMU2.PDON (\_SB.PMU2.PGPU)

    SCLK (GCLK)
  }

  Method (_PS3, 0, Serialized) {
    \_SB.PMU2.PDOF (\_SB.PMU2.PGPU)
  }

  Method (_PSC, 0, Serialized) {
    If (\_SB.PMU2.PDST (\_SB.PMU2.PGPU)) {
      Return (0)
    }
    Return (3)
  }

  //
  // Function 0: supported functions
  // Function 1: set the GPU clock. Arg3[0] = rate in Hz.
  //             Returns the rate that was set (capped at max-frequency),
  //             or 0 on failure.
  // Function 2: get the GPU clock. Returns the rate in Hz, or 0.
  // Function 3: get the highest allowed GPU clock in Hz.
  //
  Method (_DSM, 4, Serialized) {
    If (Arg0 == GPU_DSM_UUID) {
      If (Arg1 >= GPU_DSM_REVISION) {
        Switch (ToInteger (Arg2)) {
          Case (0) {
            Return (Buffer () { 0x0F })
          }
          Case (1) {
            Return (SCLK (DerefOf (Arg3[0])))
          }
          Case (2) {
            Local0 = \_SB.SCMI.CLRG (GPU_SCMI_CLOCK_ID)
            If (DerefOf (Local0[0]) != 0) {
              Return (0)
            }
            Return (DerefOf (Local0[1]))
          }
          Case (3) {
            Return (GPU_CLOCK_MAX_HZ)
          }
        }
      }
    }
    Return (Buffer () { 0x0 })
  }
}
