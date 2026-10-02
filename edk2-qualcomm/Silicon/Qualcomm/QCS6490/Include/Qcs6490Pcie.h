/** @file
  PCIe0 of the QCS6490 (pcie@1c00000 in kodiak.dtsi, a Synopsys DesignWare
  5.00a root complex): its address map and the registers that
  Qcs6490PciHostBridgeLib (bring-up) and Qcs6490PciSegmentLib (configuration
  space access) share.

  Register names and offsets are those of Linux
  (drivers/pci/controller/dwc/pcie-designware.h, pcie-qcom.c).

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_PCIE_H_
#define QCS6490_PCIE_H_

#include <Library/IoLib.h>

//
// reg and ranges of pcie0 (kodiak.dtsi): "parf", the QMP PHY, "dbi" (0xF1D
// bytes, followed by "elbi" at 0xF20), "atu", "config", and the 32-bit MEM
// window, where the PCI address is the CPU address. The IO part of ranges
// (CPU 0x60200000 -> PCI 0) is not used: UEFI sets up no IO window.
//
#define PCIE0_SEGMENT       0
#define PCIE0_PARF_BASE     0x01C00000
#define PCIE0_PHY_BASE      0x01C06000
#define PCIE0_DBI_BASE      0x60000000
#define PCIE0_DBI_LAST_REG  0xF1C
#define PCIE0_ATU_BASE      0x60001000
#define PCIE0_CFG_BASE      0x60100000
#define PCIE0_MEM_BASE      0x60300000
#define PCIE0_MEM_LIMIT     0x63FFFFFF

//
// Buses UEFI uses: the root port is 00:00.0 and the one device below it is
// on bus 1.
//
#define PCIE0_ROOT_BUS       0
#define PCIE0_SECONDARY_BUS  1

//
// GCC_PCIE_0_GDSC and GCC_PCIE_0_BCR (gcc-sc7280.c). With the power domain
// off or the controller held in reset, the DBI does not answer and an access
// stalls the bus.
//
#define PCIE0_GCC_BCR          0x0016B000
#define PCIE0_GCC_BCR_ASSERT   BIT0
#define PCIE0_GCC_GDSCR        0x0016B004
#define PCIE0_GCC_GDSC_PWR_ON  BIT31

//
// The root port's PCI Express capability in the DBI (live board: PM at 0x40,
// MSI at 0x50, PCIe at 0x70). LNKSTA: current link speed in bits 3:0,
// negotiated width in bits 9:4, data link layer link active in bit 13
// (qcom_pcie_link_up).
//
#define PCIE0_DBI_PCIE_CAP          0x70
#define PCIE0_DBI_LNKCAP            (PCIE0_DBI_PCIE_CAP + 0x0C)
#define PCIE0_DBI_LNKSTA            (PCIE0_DBI_PCIE_CAP + 0x12)
#define PCIE0_LNKCAP_SLS(x)         ((x) & 0xF)
#define PCIE0_LNKCAP_MLW_MASK       (0x3F << 4)
#define PCIE0_LNKCAP_MLW(n)         ((UINT32)(n) << 4)
#define PCIE0_LNKSTA_CLS(x)         ((x) & 0xF)
#define PCIE0_LNKSTA_NLW(x)         (((x) >> 4) & 0x3F)
#define PCIE0_LNKSTA_DLLLA          BIT13

//
// The internal address translation unit (iATU), in the "unrolled" layout
// with its own register block ("atu"): outbound region n at n * 0x200,
// inbound region n at n * 0x200 + 0x100, 8 of each (atu size 0x1000 / 512).
// The registers of a region (PCIE_ATU_UNR_* in pcie-designware.h):
//   CTRL1:  type in bits 4:0 (MEM 0, IO 2, CFG0 4, CFG1 5)
//   CTRL2:  bit 31 region enable
//   bases, limits and targets: 32-bit halves
//
#define DW_ATU_REGIONS             8
#define DW_ATU_OB(n)               (PCIE0_ATU_BASE + (UINTN)(n) * 0x200)
#define DW_ATU_IB(n)               (PCIE0_ATU_BASE + (UINTN)(n) * 0x200 + 0x100)
#define DW_ATU_CTRL1               0x00
#define DW_ATU_CTRL2               0x04
#define DW_ATU_LWR_BASE            0x08
#define DW_ATU_UPR_BASE            0x0C
#define DW_ATU_LIMIT               0x10
#define DW_ATU_LWR_TARGET          0x14
#define DW_ATU_UPR_TARGET          0x18
#define DW_ATU_UPR_LIMIT           0x20
#define DW_ATU_TYPE_MEM            0x0
#define DW_ATU_TYPE_IO             0x2
#define DW_ATU_TYPE_CFG0           0x4
#define DW_ATU_TYPE_CFG1           0x5
#define DW_ATU_ENABLE              BIT31
#define DW_ATU_TARGET_BDF(B, D, F) \
  (((UINT32)(B) << 24) | ((UINT32)(D) << 19) | ((UINT32)(F) << 16))

//
// Outbound region 0 is the configuration window (dw_pcie_other_conf_map_bus
// reprograms it for each target); region 1 is the MEM window
// (dw_pcie_iatu_setup starts the MEM/IO windows at index 1).
//
#define DW_ATU_REGION_CFG  0
#define DW_ATU_REGION_MEM  1

//
// How long the enable of a newly programmed region may take to read back
// (LINK_WAIT_MAX_IATU_RETRIES x LINK_WAIT_IATU ms, pcie-designware.h).
//
#define DW_ATU_ENABLE_RETRIES   5
#define DW_ATU_ENABLE_RETRY_US  9000

/**
  Returns whether the PCIe0 controller registers (DBI, iATU) can be accessed:
  its power domain is on and it is not held in reset. Reads GCC registers
  only.

  @return  TRUE if they can.
**/
STATIC inline
BOOLEAN
Qcs6490Pcie0Accessible (
  VOID
  )
{
  if ((MmioRead32 (PCIE0_GCC_GDSCR) & PCIE0_GCC_GDSC_PWR_ON) == 0) {
    return FALSE;
  }

  return (MmioRead32 (PCIE0_GCC_BCR) & PCIE0_GCC_BCR_ASSERT) == 0;
}

/**
  Returns whether the PCIe0 link is up (qcom_pcie_link_up: LNKSTA data link
  layer link active). A configuration request sent below the root port while
  the link is down can raise an SError (pcie-designware-host.c
  dw_pcie_other_conf_map_bus); the DBI is read only when the controller can
  be accessed.

  @return  TRUE if it is.
**/
STATIC inline
BOOLEAN
Qcs6490Pcie0LinkUp (
  VOID
  )
{
  if (!Qcs6490Pcie0Accessible ()) {
    return FALSE;
  }

  return (MmioRead16 (PCIE0_DBI_BASE + PCIE0_DBI_LNKSTA) & PCIE0_LNKSTA_DLLLA) != 0;
}

#endif // QCS6490_PCIE_H_
