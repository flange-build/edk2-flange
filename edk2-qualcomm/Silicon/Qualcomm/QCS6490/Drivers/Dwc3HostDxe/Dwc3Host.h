/** @file
  QCS6490 secondary USB controller (usb_2) in host mode: register map of the
  controller, its HS PHY, and the GCC clocks, power domain and resets it
  needs.

  usb_2 is a Synopsys DWC_usb3 3.30a dual-role core, USB 2.0 only (no
  SuperSpeed PHY), with a Synopsys femto HS PHY behind a UTMI interface. Its
  xHCI register file is the first 32 KiB of the controller; the DWC3 global
  registers follow at +0xC100 and the Qualcomm glue (QSCRATCH) at +0xF8800.

  Register names follow Linux 7.0: drivers/usb/dwc3/core.h, gadget.h,
  dwc3-qcom.c, drivers/phy/qualcomm/phy-qcom-snps-femto-v2.c,
  drivers/clk/qcom/gcc-sc7280.c, clk-alpha-pll.c and clk-rcg2.c, and the
  usb_2 and usb_2_hsphy nodes of kodiak.dtsi.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef DWC3_HOST_H_
#define DWC3_HOST_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Qcs6490GccLib.h>
#include <Library/Qcs6490RpmhLib.h>
#include <Library/Qcs6490TlmmLib.h>
#include <Library/TimerLib.h>

//
// GCC registers of usb_2 (gcc-sc7280.c).
//
#define GCC_USB30_SEC_BCR                 (GCC_BASE + 0x9E000)
#define GCC_USB30_SEC_GDSCR               (GCC_BASE + 0x9E004)
#define GCC_USB30_SEC_MASTER_CBCR         (GCC_BASE + 0x9E010)
#define GCC_USB30_SEC_SLEEP_CBCR          (GCC_BASE + 0x9E018)
#define GCC_USB30_SEC_MOCK_UTMI_CBCR      (GCC_BASE + 0x9E01C)
#define GCC_USB30_SEC_MASTER_CMD_RCGR     (GCC_BASE + 0x9E020)
#define GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR  (GCC_BASE + 0x9E038)
#define GCC_USB30_SEC_MOCK_UTMI_POSTDIV   (GCC_BASE + 0x9E050)
#define GCC_CFG_NOC_USB3_SEC_AXI_CBCR     (GCC_BASE + 0x9E07C)
#define GCC_AGGRE_USB3_SEC_AXI_CBCR       (GCC_BASE + 0x9E080)
#define GCC_QUSB2PHY_SEC_BCR              (GCC_BASE + 0x12004)

//
// GPLL0, the source of the master clock: a Lucid PLL in FSM vote mode
// (gcc_gpll0, clk-alpha-pll.c clk_trion_pll_enable).
//
#define GCC_GPLL0_MODE              (GCC_BASE + 0x0)
#define GCC_GPLL0_MODE_VOTE_FSM_ENA  BIT20
#define GCC_GPLL0_MODE_ACTIVE_FLAG   BIT30
#define GCC_GPLL0_ENA_VOTE          (GCC_BASE + 0x52010)
#define GCC_GPLL0_ENA               BIT0
#define GCC_GPLL0_ACTIVE_TIMEOUT_US  1500     // wait_for_pll

//
// Branch clock control register (clk-branch.h).
//
#define CBCR_CLK_ENABLE  BIT0

//
// Root clock generator, clk_rcg2 layout: CFG_RCGR after CMD_RCGR. CFG is
// SRC_SEL << 8 | (2 x divider - 1); a SRC_DIV of 0 also means undivided.
//
#define RCG_CFG_OFFSET         0x4
#define RCG_CFG_SRC_DIV_MASK   0x1F
#define RCG_CFG_SRC_SEL_SHIFT  8
#define RCG_CFG_SRC_SEL_MASK   (0x7 << RCG_CFG_SRC_SEL_SHIFT)

//
// Root configurations. The master root's table (ftbl_gcc_usb30_sec_master_
// clk_src) tops out at 120 MHz, GPLL0_OUT_EVEN (300 MHz, source 6 of
// gcc_parent_map_5) / 2.5, which is what Linux picks for the 200 MHz of the
// DT's assigned-clock-rates. The mock UTMI root runs from XO (source 0 of
// gcc_parent_map_3) undivided, 19.2 MHz.
//
#define USB30_SEC_MASTER_CFG_120MHZ     0x604
#define USB30_SEC_MOCK_UTMI_CFG_19_2MHZ  0x001
#define USB30_SEC_MOCK_UTMI_SRC_XO      0

//
// Resets: Linux holds the controller BCR for usleep_range (10, 1000)
// (dwc3-qcom.c) and the PHY BCR for usleep_range (100, 150)
// (phy-qcom-snps-femto-v2.c), and touches the block right after the
// release; the short wait after the release is only a margin.
//
#define USB30_SEC_BCR_ASSERT_US     10
#define USB30_SEC_BCR_DEASSERT_US   10
#define QUSB2PHY_SEC_BCR_ASSERT_US  150
#define QUSB2PHY_SEC_BCR_DEASSERT_US  10

//
// RPMh resources: the PHY's supplies (kodiak.dtsi usb_2_hsphy, board
// vdda-pll-supply, vdda18-supply and vdda33-supply), addresses from the
// command DB of this board's firmware.
//
#define RPMH_ADDR_LDOC10  0x40B00   // PM8350C L10: vdda-pll, 0.88 V
#define RPMH_ADDR_LDOC1   0x42300   // PM8350C L1: vdda18, 1.8 V
#define RPMH_ADDR_LDOB2   0x40C00   // PM7325 L2: vdda33, 3.072 V

//
// VRM registers of a regulator resource (qcom-rpmh-regulator.c).
//
#define RPMH_VRM_VOLTAGE         0x0     // mV
#define RPMH_VRM_ENABLE          0x4
#define RPMH_VRM_MODE            0x8
#define RPMH_PMIC5_LDO_MODE_HPM  7

//
// The RPMh acknowledgement of an enable already covers the regulator's
// settling; the DT has no ramp delay for these rails. The wait is a margin.
//
#define DWC3_HOST_RAIL_SETTLE_US  100

//
// Board GPIOs (PcdUsbSecondaryPowerGpios): driven high at 2 mA, as the
// vendor DT's usb2_1p8_vreg pin configuration does, then given time to
// ramp (no DT value; a margin for a load switch).
//
#define DWC3_HOST_GPIO_LIST_END   0xFFFF
#define DWC3_HOST_GPIO_DRIVE_MA   2
#define DWC3_HOST_GPIO_RAMP_US    1000

//
// usb_2 (kodiak.dtsi usb@8c00000). The xHCI register file is what XhciDxe
// gets (DWC3_XHCI_REGS_END 0x7fff).
//
#define USB2_DWC3_BASE  0x08C00000
#define USB2_XHCI_SIZE  0x8000

//
// DWC3 global and device registers (core.h), offsets from USB2_DWC3_BASE.
//
#define DWC3_GCTL             0xC110
#define DWC3_GSTS             0xC118
#define DWC3_GUCTL1           0xC11C
#define DWC3_GSNPSID          0xC120
#define DWC3_GBUSERRADDR0     0xC130
#define DWC3_GBUSERRADDR1     0xC134
#define DWC3_GHWPARAMS0       0xC140
#define DWC3_GHWPARAMS1       0xC144
#define DWC3_GHWPARAMS3       0xC14C
#define DWC3_GHWPARAMS6       0xC158
#define DWC3_GUCTL2           0xC19C
#define DWC3_GUSB2PHYCFG0     0xC200
#define DWC3_GUSB3PIPECTL0    0xC2C0
#define DWC3_DCTL             0xC704

#define DWC3_GSNPSID_MASK      0xFFFF0000
#define DWC3_GSNPSREV_MASK     0x0000FFFF
#define DWC3_GSNPSID_DWC_USB3  0x55330000

//
// Core revisions the Linux driver tests (DWC3_REVISION_*, low half).
//
#define DWC3_REVISION_190A  0x190A
#define DWC3_REVISION_194A  0x194A
#define DWC3_REVISION_210A  0x210A
#define DWC3_REVISION_250A  0x250A
#define DWC3_REVISION_290A  0x290A
#define DWC3_REVISION_300A  0x300A
#define DWC3_REVISION_310A  0x310A
#define DWC3_REVISION_320A  0x320A

#define DWC3_GCTL_U2RSTECN         BIT16
#define DWC3_GCTL_PRTCAPDIR_SHIFT  12
#define DWC3_GCTL_PRTCAPDIR_MASK   (0x3 << DWC3_GCTL_PRTCAPDIR_SHIFT)
#define DWC3_GCTL_PRTCAP_HOST      1
#define DWC3_GCTL_SOFITPSYNC       BIT10
#define DWC3_GCTL_SCALEDOWN_MASK   (0x3 << 4)
#define DWC3_GCTL_DISSCRAMBLE      BIT3
#define DWC3_GCTL_GBLHIBERNATIONEN  BIT1
#define DWC3_GCTL_DSBLCLKGTNG      BIT0

#define DWC3_GSTS_CURMOD_MASK      0x3
#define DWC3_GSTS_BUS_ERR_ADDR_VLD  BIT4

#define DWC3_GUCTL1_DEV_DECOUPLE_L1L2_EVT        BIT31
#define DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK  BIT26
#define DWC3_GUCTL1_DEV_L1_EXIT_BY_HW            BIT24

#define DWC3_GUCTL2_LC_TIMER          BIT19
#define DWC3_GUCTL2_RST_ACTBITLATER   BIT14

#define DWC3_GHWPARAMS0_MODE_MASK    0x3
#define DWC3_GHWPARAMS0_MODE_GADGET  0
#define DWC3_GHWPARAMS0_MODE_HOST    1
#define DWC3_GHWPARAMS0_MODE_DRD     2

#define DWC3_GHWPARAMS1_EN_PWROPT_SHIFT  24
#define DWC3_GHWPARAMS1_EN_PWROPT_MASK   0x3
#define DWC3_GHWPARAMS1_EN_PWROPT_CLK    1
#define DWC3_GHWPARAMS1_EN_PWROPT_HIB    2

#define DWC3_GHWPARAMS3_SSPHY_IFC_MASK   0x3
#define DWC3_GHWPARAMS3_HSPHY_IFC_SHIFT  2
#define DWC3_GHWPARAMS3_HSPHY_IFC_MASK   0x3

#define DWC3_GUSB2PHYCFG_ENBLSLPM  BIT8
#define DWC3_GUSB2PHYCFG_SUSPHY    BIT6

#define DWC3_GUSB3PIPECTL_UX_EXIT_PX  BIT27
#define DWC3_GUSB3PIPECTL_SUSPHY      BIT17

#define DWC3_DCTL_RUN_STOP           BIT31
#define DWC3_DCTL_CSFTRST            BIT30
#define DWC3_DCTL_ULSTCHNGREQ_MASK   (0xF << 5)

//
// dwc3_core_soft_reset: 1000 polls 1 us apart (DWC_usb3, not DWC_usb31).
//
#define DWC3_CSFTRST_TIMEOUT_US  1000

//
// xHCI registers read back for the log and a sanity check (xHCI 1.1
// section 5): capability registers from USB2_DWC3_BASE, operational
// registers from USB2_DWC3_BASE + CAPLENGTH.
//
#define XHC_CAPLENGTH_HCIVERSION  0x00
#define XHC_HCSPARAMS1            0x04
#define XHC_HCCPARAMS1            0x10
#define XHC_OP_USBCMD             0x00
#define XHC_OP_USBSTS             0x04
#define XHC_OP_PORTSC1            0x400

#define XHC_CAPLENGTH_MIN         0x20     // the capability registers
#define XHC_HCIVERSION_MIN        0x0100
#define XHC_HCSPARAMS1_MAX_PORTS(x)  (((x) >> 24) & 0xFF)
#define XHC_HCCPARAMS1_AC64       BIT0
#define XHC_HCCPARAMS1_PPC        BIT3
#define XHC_USBSTS_HCH            BIT0
#define XHC_USBSTS_HSE            BIT2
#define XHC_USBSTS_HCE            BIT12

//
// How often the controller is checked for a host system error, a host
// controller error or a bus error while UEFI drives it, in 100 ns units.
//
#define DWC3_HOST_WATCH_PERIOD  EFI_TIMER_PERIOD_MILLISECONDS (500)

//
// HS PHY (kodiak.dtsi usb_2_hsphy). Its registers are eight bits wide: a
// 32-bit read returns the byte in all four lanes and only the low byte
// counts. Accesses are 32-bit read-modify-write plus a read-back, as Linux
// does them (qcom_snps_hsphy_write_mask).
//
#define USB2_HS_PHY_BASE  0x088E4000

#define USB2_PHY_USB_PHY_UTMI_CTRL0           0x3C
#define USB2_PHY_UTMI_CTRL0_SLEEPM            BIT0

#define USB2_PHY_USB_PHY_UTMI_CTRL5           0x50
#define USB2_PHY_UTMI_CTRL5_POR               BIT1

#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0  0x54
#define USB2_PHY_COMMON0_SIDDQ                BIT2
#define USB2_PHY_COMMON0_FSEL_MASK            (0x7 << 4)

#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1  0x58
#define USB2_PHY_COMMON1_VBUSVLDEXTSEL0       BIT4
#define USB2_PHY_COMMON1_PLLBTUNE             BIT5

#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2  0x5C
#define USB2_PHY_COMMON2_VREGBYPASS           BIT0

#define USB2_PHY_USB_PHY_HS_PHY_CTRL1         0x60
#define USB2_PHY_CTRL1_VBUSVLDEXT0            BIT0

#define USB2_PHY_USB_PHY_HS_PHY_CTRL2         0x64
#define USB2_PHY_CTRL2_USB2_SUSPEND_N         BIT2
#define USB2_PHY_CTRL2_USB2_SUSPEND_N_SEL     BIT3

#define USB2_PHY_USB_PHY_CFG0                 0x94
#define USB2_PHY_CFG0_CMN_CTRL_OVERRIDE_EN    BIT1

#define USB2_PHY_USB_PHY_REFCLK_CTRL          0xA0
#define USB2_PHY_REFCLK_CTRL_SEL_BIT1         BIT1

//
// A write of the HS PHY init sequence: (register & ~Mask) | (Value & Mask).
//
typedef struct {
  UINT32    Offset;
  UINT32    Mask;
  UINT32    Value;
} DWC3_HOST_PHY_WRITE;

//
// The low byte Linux 7.0 leaves in an HS PHY register after its init,
// read on the running board.
//
typedef struct {
  UINT32    Offset;
  UINT8     Expected;
} DWC3_HOST_PHY_EXPECT;

//
// An LDO voted on in high power mode.
//
typedef struct {
  CONST CHAR8    *Name;
  UINT32         Addr;
  UINT32         MilliVolts;
} DWC3_HOST_RAIL;

//
// A branch clock of usb_2, turned on through its own CBCR.
//
typedef struct {
  CONST CHAR8       *Name;
  UINTN             Cbcr;
  GCC_HALT_CHECK    Halt;
} DWC3_HOST_BRANCH;

#endif // DWC3_HOST_H_
