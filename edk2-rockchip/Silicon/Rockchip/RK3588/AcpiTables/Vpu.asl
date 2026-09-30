/** @file
 *
 *  RK3588 video codecs: two RKVDEC (H.264/HEVC/VP9/AVS2) decoder cores,
 *  two RKVENC (H.264/HEVC) encoder cores and the AV1 decoder, each with
 *  its Rockchip IOMMU.
 *
 *  Every core sits in its own power domain, switched from _PS0/_PS3. The
 *  parent domains are brought up first and never switched off here:
 *
 *    VCODEC -> RKVDEC0, RKVDEC1, VENC0 -> VENC1
 *    VDPU   -> RKVDEC0, RKVDEC1, AV1   (bus interface)
 *
 *  VENC1 hangs off VENC0, so VENC0 stays on while VENC1 is in use.
 *
 *  The clocks each domain needs while it switches are ungated in _PS0
 *  (CRU gate bits from the Linux rk3588 clock driver); resets are left
 *  to the driver.
 *
 *  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiTables.h"

// CRU_GATE_CON40..68 (bit set = gated, upper 16 bits are the write mask).
OperationRegion (VCRU, SystemMemory, 0xFD7C08A0, 0x74)
Field (VCRU, DWordAcc, NoLock, WriteAsZeros) {
  VG40, 32,           // RKVDEC0, RKVDEC CCU
  VG41, 32,           // RKVDEC1
  Offset (0x10),
  VG44, 32,           // VDPU root clocks
  Offset (0x1C),
  VG47, 32,           // RKVENC0
  VG48, 32,           // RKVENC1
  Offset (0x70),
  VG68, 32            // AV1
}

#define VDPU_ROOT_GATES     0x0007    // CON44: aclk_vdpu_root, aclk_vdpu_low_root, hclk_vdpu_root
#define RKVDEC0_GATES       0x03FC    // CON40: aclk_rkvdec_ccu .. clk_rkvdec0_core
#define RKVDEC1_GATES       0x01FC    // CON41: hclk_rkvdec1 .. clk_rkvdec1_core
#define RKVENC0_GATES       0x0073    // CON47: roots, hclk/aclk_rkvenc0, clk_rkvenc0_core
#define RKVENC0_BUS_GATES   0x0033    // CON47: roots, hclk/aclk_rkvenc0 (feed RKVENC1)
#define RKVENC1_GATES       0x007F    // CON48: roots, pre gates, hclk/aclk_rkvenc1, core
#define AV1_GATES           0x003F    // CON68: roots, pre gates, aclk_av1, pclk_av1

// Brings up the VDPU bus domain and its root clocks.
Method (VDUP, 0, Serialized) {
  VG44 = VDPU_ROOT_GATES << 16
  Return (\_SB.PMU2.PDON (\_SB.PMU2.PVDP))
}

// Brings up the VCODEC domain.
Method (VCUP, 0, Serialized) {
  Return (\_SB.PMU2.PDON (\_SB.PMU2.PVCD))
}

Device (VDC0) {
  Name (_HID, "RKCP0E30")
  Name (_UID, 0)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFDC38000, 0x800)  // link, function, cache, IOMMU
    Memory32Fixed (ReadWrite, 0xFDC30000, 0x100)  // CCU, shared by both cores
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 127 }  // core
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 128 }  // IOMMU
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rk3588-vdec" },
    }
  })

  Method (_PS0, 0, Serialized) {
    VG40 = RKVDEC0_GATES << 16
    VCUP ()
    VDUP ()
    \_SB.PMU2.PDON (\_SB.PMU2.PVD0)
  }

  Method (_PS3, 0, Serialized) {
    \_SB.PMU2.PDOF (\_SB.PMU2.PVD0)
  }

  Method (_PSC, 0, Serialized) {
    If (\_SB.PMU2.PDST (\_SB.PMU2.PVD0)) {
      Return (0)
    }
    Return (3)
  }
}

Device (VDC1) {
  Name (_HID, "RKCP0E30")
  Name (_UID, 1)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    // The Rockchip kernel places this core at 0xFDC48000 (0x8000 into the
    // RKVDEC1 window, like core 0); mainline Linux uses 0xFDC40000.
    Memory32Fixed (ReadWrite, 0xFDC48000, 0x800)  // link, function, cache, IOMMU
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 129 }  // core
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 130 }  // IOMMU
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rk3588-vdec" },
    }
  })

  Method (_PS0, 0, Serialized) {
    VG41 = RKVDEC1_GATES << 16
    VCUP ()
    VDUP ()
    \_SB.PMU2.PDON (\_SB.PMU2.PVD1)
  }

  Method (_PS3, 0, Serialized) {
    \_SB.PMU2.PDOF (\_SB.PMU2.PVD1)
  }

  Method (_PSC, 0, Serialized) {
    If (\_SB.PMU2.PDST (\_SB.PMU2.PVD1)) {
      Return (0)
    }
    Return (3)
  }
}

Device (VEN0) {
  Name (_HID, "RKCP0E31")
  Name (_UID, 0)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  // Set while this device is in D0, so VEN1 knows it may not switch the
  // shared VENC0 domain off.
  Name (VACT, 0)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFDBD0000, 0x6000)  // core
    Memory32Fixed (ReadWrite, 0xFDBDF000, 0x80)    // IOMMU (two instances)
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 133 }  // core
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 131, 132 }  // IOMMU
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rkv-encoder-v2-core" },
    }
  })

  Method (_PS0, 0, Serialized) {
    VG47 = RKVENC0_GATES << 16
    VCUP ()
    \_SB.PMU2.PDON (\_SB.PMU2.PVE0)
    VACT = 1
  }

  Method (_PS3, 0, Serialized) {
    VACT = 0
    If (\_SB.PMU2.PDST (\_SB.PMU2.PVE1) == 0) {
      \_SB.PMU2.PDOF (\_SB.PMU2.PVE0)
    }
  }

  Method (_PSC, 0, Serialized) {
    If (VACT) {
      Return (0)
    }
    Return (3)
  }
}

Device (VEN1) {
  Name (_HID, "RKCP0E31")
  Name (_UID, 1)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFDBE0000, 0x6000)  // core
    Memory32Fixed (ReadWrite, 0xFDBEF000, 0x80)    // IOMMU (two instances)
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 136 }  // core
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 134, 135 }  // IOMMU
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rkv-encoder-v2-core" },
    }
  })

  Method (_PS0, 0, Serialized) {
    VG47 = RKVENC0_BUS_GATES << 16
    VG48 = RKVENC1_GATES << 16
    VCUP ()
    \_SB.PMU2.PDON (\_SB.PMU2.PVE0)
    \_SB.PMU2.PDON (\_SB.PMU2.PVE1)
  }

  Method (_PS3, 0, Serialized) {
    \_SB.PMU2.PDOF (\_SB.PMU2.PVE1)
    If (\_SB.VEN0.VACT == 0) {
      \_SB.PMU2.PDOF (\_SB.PMU2.PVE0)
    }
  }

  Method (_PSC, 0, Serialized) {
    If (\_SB.PMU2.PDST (\_SB.PMU2.PVE1)) {
      Return (0)
    }
    Return (3)
  }
}

Device (AV1D) {
  Name (_HID, "RKCP0E32")
  Name (_UID, 0)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFDC70000, 0x800)  // decoder
    Memory32Fixed (ReadWrite, 0xFDCA0000, 0x600)  // IOMMU
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 140 }  // decoder
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 141 }  // IOMMU
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rk3588-av1-vpu" },
    }
  })

  Method (_PS0, 0, Serialized) {
    VG68 = AV1_GATES << 16
    VDUP ()
    \_SB.PMU2.PDON (\_SB.PMU2.PAV1)
  }

  Method (_PS3, 0, Serialized) {
    \_SB.PMU2.PDOF (\_SB.PMU2.PAV1)
  }

  Method (_PSC, 0, Serialized) {
    If (\_SB.PMU2.PDST (\_SB.PMU2.PAV1)) {
      Return (0)
    }
    Return (3)
  }
}
