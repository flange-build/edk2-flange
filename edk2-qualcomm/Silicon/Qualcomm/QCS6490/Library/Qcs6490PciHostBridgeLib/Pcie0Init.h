/** @file
  PCIe0 bring-up of Qcs6490PciHostBridgeLib: the registers it programs and
  the interfaces between its parts.

  Register names follow Linux 7.0.2: drivers/clk/qcom/gcc-sc7280.c,
  drivers/phy/qualcomm/phy-qcom-qmp-pcie.c (and its phy-qcom-qmp*.h),
  drivers/pci/controller/dwc/pcie-qcom.c and pcie-designware.h.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef PCIE0_INIT_H_
#define PCIE0_INIT_H_

#include <Uefi.h>
#include <Library/Qcs6490GccLib.h>

#include <Qcs6490Pcie.h>

//
// GCC (gcc-sc7280.c): the PCIe0 block at 0x6B000, the PHY reset, and the
// shared NoC/TBU/reference branches PCIe0 uses.
//
#define GCC_PCIE_0_SLV_Q2A_AXI_CBCR          (GCC_BASE + 0x6B010)
#define GCC_PCIE_0_SLV_AXI_CBCR              (GCC_BASE + 0x6B014)
#define GCC_PCIE_0_MSTR_AXI_CBCR             (GCC_BASE + 0x6B01C)
#define GCC_PCIE_0_CFG_AHB_CBCR              (GCC_BASE + 0x6B024)
#define GCC_PCIE_0_AUX_CBCR                  (GCC_BASE + 0x6B028)
#define GCC_PCIE_0_PIPE_CBCR                 (GCC_BASE + 0x6B030)
#define GCC_PCIE0_PHY_RCHNG_CBCR             (GCC_BASE + 0x6B038)
#define GCC_PCIE_0_PHY_RCHNG_CMD_RCGR        (GCC_BASE + 0x6B03C)
#define GCC_PCIE_0_PIPE_MUXR                 (GCC_BASE + 0x6B054)
#define GCC_PCIE_0_AUX_CMD_RCGR              (GCC_BASE + 0x6B058)
#define GCC_AGGRE_NOC_PCIE_0_AXI_CBCR        (GCC_BASE + 0x6B080)
#define GCC_PCIE_0_PHY_BCR                   (GCC_BASE + 0x6C01C)
#define GCC_PCIE_CLKREF_EN                   (GCC_BASE + 0x8C004)
#define GCC_DDRSS_PCIE_SF_CBCR               (GCC_BASE + 0x8D080)
#define GCC_AGGRE_NOC_PCIE_CENTER_SF_AXI_CBCR  (GCC_BASE + 0x8D088)
#define GCC_AGGRE_NOC_PCIE_TBU_CBCR          (GCC_BASE + 0x90010)

#define PCIE0_CFG_RCGR  0x4    // CFG_RCGR, after CMD_RCGR

//
// GCC_PCIE_0_PIPE_CLK_SRC is a PHY mux (clk-regmap-phy-mux.c): bits 1:0 = 0
// select the PHY's pipe clock, 2 the XO.
//
#define GCC_PIPE_MUX_MASK     0x3
#define GCC_PIPE_MUX_PHY_SRC  0x0

//
// GCC_PCIE_0_GDSC wait times (gcc-sc7280.c gcc_pcie_0_gdsc).
//
#define PCIE0_GDSC_WAITS  GCC_GDSC_WAITS (0x2, 0x2, 0xF)

//
// Root configurations (gcc-sc7280.c frequency tables): the aux root on the
// XO undivided, 19.2 MHz, which is what it reads under Linux (the mainline
// device tree sets no rate for it); the PHY refgen root at 100 MHz,
// GPLL0_OUT_EVEN (300 MHz, source 6) / 3, which the PHY node's
// assigned-clock-rates asks for.
//
#define PCIE0_AUX_RCG_CFG    0x000
#define PCIE0_RCHNG_RCG_CFG  0x605

//
// PARF (pcie-qcom.c).
//
#define PARF_SYS_CTRL                      (PCIE0_PARF_BASE + 0x000)
#define PARF_SYS_CTRL_MAC_PHY_PWRDN_MUX_EN BIT29
#define PARF_PM_CTRL                       (PCIE0_PARF_BASE + 0x020)
#define PARF_PM_CTRL_REQ_NOT_ENTR_L1       BIT5
#define PARF_PHY_CTRL                      (PCIE0_PARF_BASE + 0x040)
#define PARF_PHY_CTRL_PHY_TEST_PWR_DOWN    BIT0
#define PARF_MHI_CLOCK_RESET_CTRL          (PCIE0_PARF_BASE + 0x174)
#define PARF_MHI_CLOCK_RESET_CTRL_BYPASS   BIT4
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2      (PCIE0_PARF_BASE + 0x1A8)
#define PARF_AXI_MSTR_WR_ADDR_HALT_EN      BIT31
#define PARF_LTSSM                         (PCIE0_PARF_BASE + 0x1B0)
#define PARF_LTSSM_EN                      BIT8
#define PARF_LTSSM_STATE(x)                ((x) & 0x3F)
#define PARF_DBI_BASE_ADDR_V2              (PCIE0_PARF_BASE + 0x350)
#define PARF_DBI_BASE_ADDR_V2_HI           (PCIE0_PARF_BASE + 0x354)
#define PARF_SLV_ADDR_SPACE_SIZE_V2        (PCIE0_PARF_BASE + 0x358)
#define PARF_SLV_ADDR_SPACE_SIZE_V2_HI     (PCIE0_PARF_BASE + 0x35C)
#define PARF_SLV_ADDR_SPACE_SZ             0x80000000
#define PARF_ATU_BASE_ADDR                 (PCIE0_PARF_BASE + 0x634)
#define PARF_ATU_BASE_ADDR_HI              (PCIE0_PARF_BASE + 0x638)
#define PARF_DEVICE_TYPE                   (PCIE0_PARF_BASE + 0x1000)
#define PARF_DEVICE_TYPE_RC                0x4
#define PARF_BDF_TO_SID_TABLE_N            (PCIE0_PARF_BASE + 0x2000)
#define PARF_BDF_TO_SID_ENTRIES            256
#define PARF_BDF_TO_SID_CFG                (PCIE0_PARF_BASE + 0x2C00)
#define PARF_BDF_TO_SID_BYPASS             BIT0

//
// QMP PHY, v4x1 layout (qmp_pcie_offsets_v4x1): PCS at 0x800
// (phy-qcom-qmp-pcs-v4.h, pciephy_v4_regs_layout), and the bits
// phy-qcom-qmp.h gives the PCS control registers.
//
#define QSERDES_V4_COM_CMN_STATUS        (PCIE0_PHY_BASE + 0x140)
#define QSERDES_V4_COM_RESET_SM_STATUS   (PCIE0_PHY_BASE + 0x144)
#define QSERDES_V4_COM_C_READY_STATUS    (PCIE0_PHY_BASE + 0x178)
#define QPHY_V4_PCS_SW_RESET             (PCIE0_PHY_BASE + 0x800)
#define QPHY_V4_PCS_PCS_STATUS1          (PCIE0_PHY_BASE + 0x814)
#define QPHY_V4_PCS_POWER_DOWN_CONTROL   (PCIE0_PHY_BASE + 0x840)
#define QPHY_V4_PCS_START_CONTROL        (PCIE0_PHY_BASE + 0x844)
#define QPHY_SW_RESET                    BIT0
#define QPHY_SW_PWRDN                    BIT0
#define QPHY_REFCLK_DRV_DSBL             BIT1
#define QPHY_SERDES_START                BIT0
#define QPHY_PCS_START                   BIT1
#define QPHY_PHYSTATUS                   BIT6

//
// DBI: the root port's configuration header, and port logic registers
// (pcie-designware.h).
//
#define DBI_VENDOR_ID                  (PCIE0_DBI_BASE + 0x000)
#define DBI_COMMAND                    (PCIE0_DBI_BASE + 0x004)
#define DBI_CLASS_DEVICE               (PCIE0_DBI_BASE + 0x00A)
#define DBI_BAR0                       (PCIE0_DBI_BASE + 0x010)
#define DBI_BAR1                       (PCIE0_DBI_BASE + 0x014)
#define DBI_PRIMARY_BUS                (PCIE0_DBI_BASE + 0x018)
#define DBI_INTERRUPT_LINE             (PCIE0_DBI_BASE + 0x03C)
#define DBI_LNKCAP                     (PCIE0_DBI_BASE + PCIE0_DBI_LNKCAP)
#define DBI_LNKSTA                     (PCIE0_DBI_BASE + PCIE0_DBI_LNKSTA)
#define DBI_SLTCAP                     (PCIE0_DBI_BASE + PCIE0_DBI_PCIE_CAP + 0x14)
#define DBI_SLTCAP_HPC                 BIT6
#define DBI_PORT_LINK_CONTROL          (PCIE0_DBI_BASE + 0x710)
#define PORT_LINK_DLL_LINK_EN          BIT5
#define PORT_LINK_FAST_LINK_MODE       BIT7
#define PORT_LINK_MODE_MASK            (0x3F << 16)
#define PORT_LINK_MODE_1_LANES         (0x1 << 16)
#define DBI_PORT_DEBUG0                (PCIE0_DBI_BASE + 0x728)
#define DBI_PORT_DEBUG1                (PCIE0_DBI_BASE + 0x72C)
#define PORT_LOGIC_LTSSM_STATE(x)      ((x) & 0x3F)
#define DBI_LINK_WIDTH_SPEED_CONTROL   (PCIE0_DBI_BASE + 0x80C)
#define PORT_LOGIC_SPEED_CHANGE        BIT17
#define PORT_LOGIC_LINK_WIDTH_MASK     (0x1F << 8)
#define PORT_LOGIC_LINK_WIDTH_1_LANES  (0x1 << 8)
#define DBI_GEN3_RELATED_OFF           (PCIE0_DBI_BASE + 0x890)
#define GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL  BIT0
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK (0x3 << 24)
#define GEN3_RELATED_OFF_RATE_SHADOW_SEL(n)   ((UINT32)(n) << 24)
#define DBI_GEN3_EQ_CONTROL_OFF        (PCIE0_DBI_BASE + 0x8A8)
#define GEN3_EQ_CONTROL_OFF_FB_MODE           (0xF << 0)
#define GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE BIT4
#define GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC      (0xFFFF << 8)
#define GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL BIT24
#define DBI_GEN3_EQ_FB_MODE_DIR_CHANGE_OFF  (PCIE0_DBI_BASE + 0x8AC)
#define GEN3_EQ_FMDC_MASK              0x3FFFF      // bits 17:0, the four fields below
#define GEN3_EQ_FMDC_T_MIN_PHASE23(n)  ((UINT32)(n) << 0)
#define GEN3_EQ_FMDC_N_EVALS(n)        ((UINT32)(n) << 5)
#define GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA(n)   ((UINT32)(n) << 10)
#define GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA(n)  ((UINT32)(n) << 14)
#define DBI_AMBA_ERROR_RESPONSE_DEFAULT  (PCIE0_DBI_BASE + 0x8D0)
#define AMBA_ERROR_RESPONSE_RRS_MASK     (0x3 << 3)
#define AMBA_ERROR_RESPONSE_RRS_FFFF0001 (0x2 << 3)
#define DBI_MISC_CONTROL_1             (PCIE0_DBI_BASE + 0x8BC)
#define DBI_RO_WR_EN                   BIT0
#define DBI_VERSION_NUMBER             (PCIE0_DBI_BASE + 0x8F8)
#define DBI_VERSION_TYPE               (PCIE0_DBI_BASE + 0x8FC)
#define DBI_ATU_VIEWPORT               (PCIE0_DBI_BASE + 0x900)

//
// Link speeds as LNKCAP/LNKSTA encode them.
//
#define LNK_SPEED_5_0GT   2
#define LNK_SPEED_8_0GT   3
#define LNK_SPEED_32_0GT  5

//
// The power GPIOs a board can list.
//
#define PCIE0_MAX_POWER_GPIOS  8

//
// How the board wires PCIe0 (from the PCDs).
//
typedef struct {
  UINT16    PerstGpio;                              // PERST#, active low
  UINTN     PowerGpioCount;
  UINT16    PowerGpios[PCIE0_MAX_POWER_GPIOS];      // driven high in order
  UINT32    PowerRampUs;                            // wait after each
} PCIE0_BOARD;

//
// One write of the QMP PHY init tables: a byte value written as 32 bits
// (qmp_configure_lane, writel), at an offset from PCIE0_PHY_BASE.
//
typedef struct {
  UINT16    Offset;
  UINT8     Value;
} PCIE0_PHY_INIT;

extern CONST PCIE0_PHY_INIT  gQcs6490Pcie0PhyInit[];
extern CONST UINTN           gQcs6490Pcie0PhyInitCount;

/**
  Brings PCIe0 up to a trained link, the way Linux 7.0.2 does
  (pcie-qcom.c, pcie-designware-host.c, phy-qcom-qmp-pcie.c). On failure
  the link training is left disabled and PERST# asserted.

  @param[in]  Board  How the board wires PCIe0.

  @retval EFI_SUCCESS  The link is up and the root port set up.
  @retval Other        It is not; the cause has been logged.
**/
EFI_STATUS
Qcs6490Pcie0Init (
  IN CONST PCIE0_BOARD  *Board
  );

/**
  Quiesces PCIe0 for the OS at ExitBootServices: link training off and
  PERST# asserted, as after a failed bring-up. The supplies stay on, so the
  devices on the link keep what they hold (the uPD720201 its firmware).

  @param[in]  Board  How the board wires PCIe0.
**/
VOID
Qcs6490Pcie0Quiesce (
  IN CONST PCIE0_BOARD  *Board
  );

#endif // PCIE0_INIT_H_
