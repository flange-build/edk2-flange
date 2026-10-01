/** @file
  QMP PHY init tables of PCIe0 (phy@1c06000, "qcom,sm8250-qmp-gen3x1-pcie-phy"
  in kodiak.dtsi), as Linux 7.0.2 writes them for a root complex
  (phy-qcom-qmp-pcie.c sm8250_qmp_gen3x1_pciephy_cfg, qmp_pcie_power_on):
  the common tables, then the root complex ones, each in the order SerDes,
  Tx, Rx, PCS, PCS PCIe (qmp_pcie_init_registers), with the v4x1 offsets
  (qmp_pcie_offsets_v4x1) and the register offsets of
  phy-qcom-qmp-qserdes-com-v4.h, phy-qcom-qmp-qserdes-txrx-v4.h,
  phy-qcom-qmp-pcs-v4.h and phy-qcom-qmp-pcs-pcie-v4.h.

  All 95 writes match what a running board reads back under Linux. The
  vendor kernel's refclk-always-on bit (EPCLK_ALWAYS_ON_EN in
  PCS_PCIE_ENDPOINT_REFCLK_CNTRL) is not set: mainline does not set it.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Pcie0Init.h"

CONST PCIE0_PHY_INIT  gQcs6490Pcie0PhyInit[] = {
  //
  // Common tables. SerDes (QSERDES COM, +0x000): sm8250_qmp_pcie_serdes_tbl.
  //
  { 0x094, 0x08 },  // QSERDES_V4_COM_SYSCLK_EN_SEL
  { 0x154, 0x34 },  // QSERDES_V4_COM_CLK_SELECT
  { 0x16C, 0x08 },  // QSERDES_V4_COM_CORECLK_DIV_MODE1
  { 0x058, 0x0F },  // QSERDES_V4_COM_PLL_IVCO
  { 0x0A4, 0x42 },  // QSERDES_V4_COM_LOCK_CMP_EN
  { 0x110, 0x24 },  // QSERDES_V4_COM_VCO_TUNE1_MODE0
  { 0x11C, 0x03 },  // QSERDES_V4_COM_VCO_TUNE2_MODE1
  { 0x118, 0xB4 },  // QSERDES_V4_COM_VCO_TUNE1_MODE1
  { 0x10C, 0x02 },  // QSERDES_V4_COM_VCO_TUNE_MAP
  { 0x1BC, 0x11 },  // QSERDES_V4_COM_BIN_VCOCAL_HSCLK_SEL
  { 0x0BC, 0x82 },  // QSERDES_V4_COM_DEC_START_MODE0
  { 0x0D4, 0x03 },  // QSERDES_V4_COM_DIV_FRAC_START3_MODE0
  { 0x0D0, 0x55 },  // QSERDES_V4_COM_DIV_FRAC_START2_MODE0
  { 0x0CC, 0x55 },  // QSERDES_V4_COM_DIV_FRAC_START1_MODE0
  { 0x0B0, 0x1A },  // QSERDES_V4_COM_LOCK_CMP2_MODE0
  { 0x0AC, 0x0A },  // QSERDES_V4_COM_LOCK_CMP1_MODE0
  { 0x0C4, 0x68 },  // QSERDES_V4_COM_DEC_START_MODE1
  { 0x0E0, 0x02 },  // QSERDES_V4_COM_DIV_FRAC_START3_MODE1
  { 0x0DC, 0xAA },  // QSERDES_V4_COM_DIV_FRAC_START2_MODE1
  { 0x0D8, 0xAB },  // QSERDES_V4_COM_DIV_FRAC_START1_MODE1
  { 0x0B8, 0x34 },  // QSERDES_V4_COM_LOCK_CMP2_MODE1
  { 0x0B4, 0x14 },  // QSERDES_V4_COM_LOCK_CMP1_MODE1
  { 0x158, 0x01 },  // QSERDES_V4_COM_HSCLK_SEL
  { 0x074, 0x06 },  // QSERDES_V4_COM_CP_CTRL_MODE0
  { 0x07C, 0x16 },  // QSERDES_V4_COM_PLL_RCTRL_MODE0
  { 0x084, 0x36 },  // QSERDES_V4_COM_PLL_CCTRL_MODE0
  { 0x078, 0x06 },  // QSERDES_V4_COM_CP_CTRL_MODE1
  { 0x080, 0x16 },  // QSERDES_V4_COM_PLL_RCTRL_MODE1
  { 0x088, 0x36 },  // QSERDES_V4_COM_PLL_CCTRL_MODE1
  { 0x1B0, 0x1E },  // QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE2_MODE0
  { 0x1AC, 0xCA },  // QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE1_MODE0
  { 0x1B8, 0x18 },  // QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE2_MODE1
  { 0x1B4, 0xA2 },  // QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE1_MODE1
  { 0x010, 0x01 },  // QSERDES_V4_COM_SSC_EN_CENTER
  { 0x01C, 0x31 },  // QSERDES_V4_COM_SSC_PER1
  { 0x020, 0x01 },  // QSERDES_V4_COM_SSC_PER2
  { 0x024, 0xDE },  // QSERDES_V4_COM_SSC_STEP_SIZE1_MODE0
  { 0x028, 0x07 },  // QSERDES_V4_COM_SSC_STEP_SIZE2_MODE0
  { 0x030, 0x4C },  // QSERDES_V4_COM_SSC_STEP_SIZE1_MODE1
  { 0x034, 0x06 },  // QSERDES_V4_COM_SSC_STEP_SIZE2_MODE1
  { 0x048, 0x90 },  // QSERDES_V4_COM_CLK_ENABLE1

  //
  // Tx lane (+0x200): sm8250_qmp_pcie_tx_tbl.
  //
  { 0x29C, 0x12 },  // QSERDES_V4_TX_RCV_DETECT_LVL_2
  { 0x284, 0x35 },  // QSERDES_V4_TX_LANE_MODE_1
  { 0x23C, 0x11 },  // QSERDES_V4_TX_RES_CODE_LANE_OFFSET_TX

  //
  // Rx lane (+0x400): sm8250_qmp_pcie_rx_tbl.
  //
  { 0x408, 0x0C },  // QSERDES_V4_RX_UCDR_FO_GAIN
  { 0x414, 0x03 },  // QSERDES_V4_RX_UCDR_SO_GAIN
  { 0x4DC, 0x1B },  // QSERDES_V4_RX_GM_CAL
  { 0x4FC, 0x00 },  // QSERDES_V4_RX_RX_IDAC_TSETTLE_HIGH
  { 0x4F8, 0xC0 },  // QSERDES_V4_RX_RX_IDAC_TSETTLE_LOW
  { 0x460, 0x30 },  // QSERDES_V4_RX_AUX_DATA_TCOARSE_TFINE
  { 0x4D4, 0x04 },  // QSERDES_V4_RX_VGA_CAL_CNTRL1
  { 0x4D8, 0x07 },  // QSERDES_V4_RX_VGA_CAL_CNTRL2
  { 0x434, 0x7F },  // QSERDES_V4_RX_UCDR_SO_SATURATION_AND_ENABLE
  { 0x444, 0x70 },  // QSERDES_V4_RX_UCDR_PI_CONTROLS
  { 0x4EC, 0x0E },  // QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL2
  { 0x4F0, 0x4A },  // QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL3
  { 0x4F4, 0x0F },  // QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL4
  { 0x51C, 0x03 },  // QSERDES_V4_RX_SIGDET_CNTRL
  { 0x518, 0x1C },  // QSERDES_V4_RX_SIGDET_ENABLES
  { 0x524, 0x1E },  // QSERDES_V4_RX_SIGDET_DEGLITCH_CNTRL
  { 0x510, 0x17 },  // QSERDES_V4_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1
  { 0x598, 0xD4 },  // QSERDES_V4_RX_RX_MODE_10_LOW
  { 0x59C, 0x54 },  // QSERDES_V4_RX_RX_MODE_10_HIGH
  { 0x5A0, 0xDB },  // QSERDES_V4_RX_RX_MODE_10_HIGH2
  { 0x5A4, 0x3B },  // QSERDES_V4_RX_RX_MODE_10_HIGH3
  { 0x5A8, 0x31 },  // QSERDES_V4_RX_RX_MODE_10_HIGH4
  { 0x584, 0x24 },  // QSERDES_V4_RX_RX_MODE_01_LOW
  { 0x578, 0xFF },  // QSERDES_V4_RX_RX_MODE_00_HIGH2
  { 0x57C, 0x7F },  // QSERDES_V4_RX_RX_MODE_00_HIGH3
  { 0x5BC, 0x0C },  // QSERDES_V4_RX_DCC_CTRL1
  { 0x588, 0xE4 },  // QSERDES_V4_RX_RX_MODE_01_HIGH
  { 0x58C, 0xEC },  // QSERDES_V4_RX_RX_MODE_01_HIGH2
  { 0x590, 0x3B },  // QSERDES_V4_RX_RX_MODE_01_HIGH3
  { 0x594, 0x36 },  // QSERDES_V4_RX_RX_MODE_01_HIGH4

  //
  // PCS (+0x800): sm8250_qmp_pcie_pcs_tbl.
  //
  { 0x9A4, 0x01 },  // QPHY_V4_PCS_P2U3_WAKEUP_DLY_TIME_AUXCLK_L
  { 0x988, 0x77 },  // QPHY_V4_PCS_RX_SIGDET_LVL
  { 0x998, 0x0B },  // QPHY_V4_PCS_RATE_SLEW_CNTRL1

  //
  // PCS PCIe (+0xC00): sm8250_qmp_pcie_pcs_misc_tbl.
  //
  { 0xC90, 0x00 },  // QPHY_V4_PCS_PCIE_OSC_DTCT_ACTIONS
  { 0xC40, 0x01 },  // QPHY_V4_PCS_PCIE_L1P1_WAKEUP_DLY_TIME_AUXCLK_L
  { 0xC48, 0x01 },  // QPHY_V4_PCS_PCIE_L1P2_WAKEUP_DLY_TIME_AUXCLK_L
  { 0xCB4, 0x33 },  // QPHY_V4_PCS_PCIE_PRESET_P6_P7_PRE
  { 0xCBC, 0x00 },  // QPHY_V4_PCS_PCIE_PRESET_P10_PRE
  { 0xCE0, 0x58 },  // QPHY_V4_PCS_PCIE_PRESET_P10_POST
  { 0xC1C, 0xC1 },  // QPHY_V4_PCS_PCIE_ENDPOINT_REFCLK_DRIVE

  //
  // Root complex tables (tbls_rc), applied after the common ones. SerDes:
  // sm8250_qmp_gen3x1_pcie_serdes_tbl.
  //
  { 0x050, 0x07 },  // QSERDES_V4_COM_SYSCLK_BUF_ENABLE

  //
  // Rx lane: sm8250_qmp_gen3x1_pcie_rx_tbl.
  //
  { 0x464, 0x00 },  // QSERDES_V4_RX_RCLK_AUXDATA_SEL
  { 0x4E8, 0x00 },  // QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL1
  { 0x5B4, 0x04 },  // QSERDES_V4_RX_DFE_EN_TIMER
  { 0x570, 0x3F },  // QSERDES_V4_RX_RX_MODE_00_LOW
  { 0x580, 0x14 },  // QSERDES_V4_RX_RX_MODE_00_HIGH4
  { 0x5B8, 0x30 },  // QSERDES_V4_RX_DFE_CTLE_POST_CAL_OFFSET

  //
  // PCS: sm8250_qmp_gen3x1_pcie_pcs_tbl.
  //
  { 0x8DC, 0x0D },  // QPHY_V4_PCS_REFGEN_REQ_CONFIG1
  { 0x9EC, 0x12 },  // QPHY_V4_PCS_EQ_CONFIG5

  //
  // PCS PCIe: sm8250_qmp_gen3x1_pcie_pcs_misc_tbl.
  //
  { 0xC50, 0x00 },  // QPHY_V4_PCS_PCIE_INT_AUX_CLK_CONFIG1
  { 0xCA4, 0x0F },  // QPHY_V4_PCS_PCIE_EQ_CONFIG2
};

CONST UINTN  gQcs6490Pcie0PhyInitCount = ARRAY_SIZE (gQcs6490Pcie0PhyInit);
