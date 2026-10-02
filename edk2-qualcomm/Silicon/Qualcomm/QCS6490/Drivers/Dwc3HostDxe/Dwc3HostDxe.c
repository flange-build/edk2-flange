/** @file
  Brings up the QCS6490 secondary USB controller (usb_2, USB 2.0 only) in
  host mode and hands its xHCI to XhciDxe as a non-discoverable device.

  Nothing before UEFI touches usb_2, so the driver does what Linux 7.0 does
  when it probes the controller with dr_mode = "host", in the same order:

  1. Board: the GPIOs of PcdUsbSecondaryPowerGpios driven high (the vendor
     DT drives gpio119, "usb2_1p8_vreg", for this port).
  2. RPMh: the HS PHY supplies L10C, L1C and L2B on, in high power mode
     (phy-qcom-snps-femto-v2.c enables them in phy_init).
  3. Clock rates before the power domain (of_clk_set_defaults runs before
     dev_pm_domain_attach): the master root at 120 MHz; the mock UTMI root
     on XO undivided, which is where the boot firmware leaves it.
  4. The usb30_sec GDSC (gdsc.c), the controller BCR pulse and the five
     branch clocks in the order of the DT's clocks property
     (dwc3-qcom.c dwc3_qcom_probe).
  5. DWC3 core (core.c dwc3_core_init): GSNPSID, PHY interface set up with
     the PHY suspend bits off, then the HS PHY init of
     phy-qcom-snps-femto-v2.c qcom_snps_hsphy_init, USB3 PHY suspend on,
     the device-side soft reset (DCTL.CSFTRST), GCTL, GUCTL2 and GUCTL1.
  6. Host mode (dwc3_set_prtcap): PHY suspend off, PRTCAPDIR = host.
  7. RegisterNonDiscoverableMmioDevice: the 32 KiB xHCI window, DMA not
     coherent (no dma-coherent in kodiak.dtsi).

  XhciDxe gets the controller's PCI I/O without dual address cycles, so
  that all its DMA stays below 4 GiB, bounced there by
  NonDiscoverablePciDeviceDxe like the UFS's, whose driver never turns them
  on. DMA above 4 GiB through the apps SMMU under Gunyah is not known to
  work here: its first use, by a boot loader whose buffers sit at the top
  of DRAM, had its reads time out and the controller stop answering
  commands. It also keeps
  the event ring segment table below 4 GiB: XhciDxe writes ERSTBA low half
  first, and this core starts fetching the table when the low half is
  written (Linux sets XHCI_WRITE_64_HI_LO for every DWC3, host.c).

  The xHCI itself (HCRST, rings, ports) is XhciDxe's, as it is xhci-plat's
  in Linux. Nothing is done at ExitBootServices: XhciDxe halts the
  controller, so it does no more DMA, and Linux pulses both BCRs and
  reprograms the clocks, the PHY and the core when it probes. A failure
  leaves the block as it got and registers nothing.

  While UEFI drives the controller, it is checked every half second for a
  host system error, a host controller error or an AXI bus error, which
  are logged with the address the core recorded.

  Two things this driver relies on from elsewhere:
  - SmmuDxe gives the controller's stream (SID 0xa0, mask 0) a bypass entry;
    under Gunyah the apps SMMU faults a stream nobody set up.
  - CX is at a level the 120 MHz master clock runs at. Linux asks for NOM
    (required-opps). UEFI's level is set by the boot firmware and cannot be
    read back over RPMh here, and a vote from this DRV could lower it for
    UFS and the display, so the driver does not vote CX.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Dwc3Host.h"

#include <Guid/EventGroup.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/PciIo.h>

//
// The xHCI's handle, its PCI I/O once NonDiscoverablePciDeviceDxe has
// installed it, and that PCI I/O's own Attributes(), which
// Dwc3HostPciIoAttributes() filters.
//
STATIC EFI_HANDLE                      mXhciHandle;
STATIC EFI_PCI_IO_PROTOCOL             *mXhciPciIo;
STATIC EFI_PCI_IO_PROTOCOL_ATTRIBUTES  mXhciPciIoAttributes;
STATIC EFI_EVENT                       mPciIoEvent;
STATIC VOID                            *mPciIoRegistration;

STATIC EFI_EVENT  mWatchEvent;
STATIC EFI_EVENT  mExitBootServicesEvent;
STATIC UINT32     mWatchReported;

//
// The HS PHY supplies of the board DT (usb_2_hsphy vdda-pll-supply,
// vdda18-supply, vdda33-supply), each at the voltage its regulator is named
// for: vreg_l10c_0p88, vreg_l1c_1p8, vreg_l2b_3p072. Linux 7.0 votes the
// DT minimum instead (720 mV on L10C, 2704 mV on L2B) and the vendor kernel
// 880/1800/3104 mV; RPMh keeps the highest vote of all its clients.
//
STATIC CONST DWC3_HOST_RAIL  mRails[] = {
  { "L10C (vdda-pll)", RPMH_ADDR_LDOC10, 880  },
  { "L1C (vdda18)",    RPMH_ADDR_LDOC1,  1800 },
  { "L2B (vdda33)",    RPMH_ADDR_LDOB2,  3072 },
};

//
// The usb_2 clocks in the order of its clocks property (cfg_noc, core,
// iface, sleep, mock_utmi), which is the order clk_bulk_prepare_enable
// turns them on, with each branch's halt check (gcc-sc7280.c). The two NoC
// branches have hardware clock gating (hwcg_bit 1); the library skips the
// halt check when it is on, as clk_branch2 does.
//
STATIC CONST DWC3_HOST_BRANCH  mBranches[] = {
  { "gcc_cfg_noc_usb3_sec_axi_clk", GCC_CFG_NOC_USB3_SEC_AXI_CBCR, GccHaltVoted },
  { "gcc_usb30_sec_master_clk",     GCC_USB30_SEC_MASTER_CBCR,     GccHalt      },
  { "gcc_aggre_usb3_sec_axi_clk",   GCC_AGGRE_USB3_SEC_AXI_CBCR,   GccHaltVoted },
  { "gcc_usb30_sec_sleep_clk",      GCC_USB30_SEC_SLEEP_CBCR,      GccHalt      },
  { "gcc_usb30_sec_mock_utmi_clk",  GCC_USB30_SEC_MOCK_UTMI_CBCR,  GccHalt      },
};

//
// qcom_snps_hsphy_init after the PHY reset, in order. The board DT has no
// tuning properties for usb_2_hsphy, so no override register is written.
//
STATIC CONST DWC3_HOST_PHY_WRITE  mHsPhyInit[] = {
  //
  // Common block under software control, power-on reset asserted, the
  // reference clock frequency select to 0 (19.2 MHz), PLL B tune.
  //
  { USB2_PHY_USB_PHY_CFG0,                USB2_PHY_CFG0_CMN_CTRL_OVERRIDE_EN, USB2_PHY_CFG0_CMN_CTRL_OVERRIDE_EN },
  { USB2_PHY_USB_PHY_UTMI_CTRL5,          USB2_PHY_UTMI_CTRL5_POR,            USB2_PHY_UTMI_CTRL5_POR            },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, USB2_PHY_COMMON0_FSEL_MASK,         0                                  },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1, USB2_PHY_COMMON1_PLLBTUNE,          USB2_PHY_COMMON1_PLLBTUNE          },
  //
  // REFCLK_SEL: Linux passes REFCLK_SEL_DEFAULT (2) as the mask and
  // REFCLK_SEL_MASK (3) as the value, which sets bit 1 and leaves bit 0.
  // Written here as what it does.
  //
  { USB2_PHY_USB_PHY_REFCLK_CTRL,         USB2_PHY_REFCLK_CTRL_SEL_BIT1,      USB2_PHY_REFCLK_CTRL_SEL_BIT1      },
  //
  // VBUS valid from the override, and the override set: a host has no
  // VBUS input to the PHY.
  //
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1, USB2_PHY_COMMON1_VBUSVLDEXTSEL0,    USB2_PHY_COMMON1_VBUSVLDEXTSEL0    },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL1,        USB2_PHY_CTRL1_VBUSVLDEXT0,         USB2_PHY_CTRL1_VBUSVLDEXT0         },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2, USB2_PHY_COMMON2_VREGBYPASS,        USB2_PHY_COMMON2_VREGBYPASS        },
  {
    USB2_PHY_USB_PHY_HS_PHY_CTRL2,
    USB2_PHY_CTRL2_USB2_SUSPEND_N_SEL | USB2_PHY_CTRL2_USB2_SUSPEND_N,
    USB2_PHY_CTRL2_USB2_SUSPEND_N_SEL | USB2_PHY_CTRL2_USB2_SUSPEND_N
  },
  { USB2_PHY_USB_PHY_UTMI_CTRL0,          USB2_PHY_UTMI_CTRL0_SLEEPM,         USB2_PHY_UTMI_CTRL0_SLEEPM         },
  //
  // Analog powered (SIDDQ off), power-on reset released, suspend back to
  // the UTMI interface, common block back to the hardware.
  //
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, USB2_PHY_COMMON0_SIDDQ,             0                                  },
  { USB2_PHY_USB_PHY_UTMI_CTRL5,          USB2_PHY_UTMI_CTRL5_POR,            0                                  },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL2,        USB2_PHY_CTRL2_USB2_SUSPEND_N_SEL,  0                                  },
  { USB2_PHY_USB_PHY_CFG0,                USB2_PHY_CFG0_CMN_CTRL_OVERRIDE_EN, 0                                  },
};

//
// What Linux 7.0 leaves in the registers it writes (low byte, read on the
// running board with usb_2 in host mode). The bits the sequence above
// writes must match; the other bits are reset values and are compared for
// the log only.
//
STATIC CONST DWC3_HOST_PHY_EXPECT  mHsPhyExpected[] = {
  { USB2_PHY_USB_PHY_UTMI_CTRL0,          0x01 },
  { USB2_PHY_USB_PHY_UTMI_CTRL5,          0x00 },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, 0x88 },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1, 0x3B },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2, 0xC1 },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL1,        0x01 },
  { USB2_PHY_USB_PHY_HS_PHY_CTRL2,        0x06 },
  { USB2_PHY_USB_PHY_CFG0,                0x00 },
  { USB2_PHY_USB_PHY_REFCLK_CTRL,         0x02 },
};

/**
  Logs the low bytes of the HS PHY registers the init sequence writes.

  @param[in]  When  A label for the log line.
**/
STATIC
VOID
HsPhyLog (
  IN CONST CHAR8  *When
  )
{
  UINT8  Bytes[ARRAY_SIZE (mHsPhyExpected)];
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mHsPhyExpected); Index++) {
    Bytes[Index] = (UINT8)MmioRead32 (USB2_HS_PHY_BASE + mHsPhyExpected[Index].Offset);
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] 3c=%02x 50=%02x 54=%02x 58=%02x 5c=%02x 60=%02x 64=%02x 94=%02x a0=%02x\n",
    __func__,
    When,
    Bytes[0],
    Bytes[1],
    Bytes[2],
    Bytes[3],
    Bytes[4],
    Bytes[5],
    Bytes[6],
    Bytes[7],
    Bytes[8]
    ));
}

/**
  Logs the DWC3 registers this driver sets up, and DCTL and GSTS.

  @param[in]  Level  The debug level.
  @param[in]  When   A label for the log line.
**/
STATIC
VOID
Dwc3Log (
  IN UINTN        Level,
  IN CONST CHAR8  *When
  )
{
  DEBUG ((
    Level,
    "%a: [%a] GCTL 0x%08x GSTS 0x%08x GUCTL1 0x%08x GUCTL2 0x%08x GUSB2PHYCFG 0x%08x GUSB3PIPECTL 0x%08x DCTL 0x%08x\n",
    __func__,
    When,
    MmioRead32 (USB2_DWC3_BASE + DWC3_GCTL),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GSTS),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GUCTL1),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GUCTL2),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GUSB2PHYCFG0),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GUSB3PIPECTL0),
    MmioRead32 (USB2_DWC3_BASE + DWC3_DCTL)
    ));
}

/**
  Logs the usb_2 power domain, resets and clocks, for a failure.
**/
STATIC
VOID
GccLogUsb2 (
  VOID
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "%a: GDSCR 0x%08x BCR 0x%x PHY BCR 0x%x master RCG 0x%x/0x%x mock RCG 0x%x/0x%x\n",
    __func__,
    MmioRead32 (GCC_USB30_SEC_GDSCR),
    MmioRead32 (GCC_USB30_SEC_BCR),
    MmioRead32 (GCC_QUSB2PHY_SEC_BCR),
    MmioRead32 (GCC_USB30_SEC_MASTER_CMD_RCGR),
    MmioRead32 (GCC_USB30_SEC_MASTER_CMD_RCGR + RCG_CFG_OFFSET),
    MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR),
    MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR + RCG_CFG_OFFSET)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "%a: CBCR cfg_noc 0x%08x master 0x%08x aggre 0x%08x sleep 0x%08x mock_utmi 0x%08x\n",
    __func__,
    MmioRead32 (GCC_CFG_NOC_USB3_SEC_AXI_CBCR),
    MmioRead32 (GCC_USB30_SEC_MASTER_CBCR),
    MmioRead32 (GCC_AGGRE_USB3_SEC_AXI_CBCR),
    MmioRead32 (GCC_USB30_SEC_SLEEP_CBCR),
    MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_CBCR)
    ));
}

/**
  Drives the board GPIOs of PcdUsbSecondaryPowerGpios high, skipping those
  already driven high, and waits for the supplies they switch to ramp when
  any was changed.

  @retval EFI_SUCCESS            Driven, or nothing to do.
  @retval EFI_INVALID_PARAMETER  The PCD names a GPIO that does not exist.
**/
STATIC
EFI_STATUS
Dwc3HostPowerGpios (
  VOID
  )
{
  CONST UINT8  *List;
  UINTN        Count;
  UINTN        Index;
  UINT16       Gpio;
  BOOLEAN      Changed;
  EFI_STATUS   Status;

  //
  // A UINT16 list in a byte array, so not necessarily aligned.
  //
  List    = (CONST UINT8 *)PcdGetPtr (PcdUsbSecondaryPowerGpios);
  Count   = PcdGetSize (PcdUsbSecondaryPowerGpios) / sizeof (UINT16);
  Changed = FALSE;

  for (Index = 0; Index < Count; Index++) {
    Gpio = ReadUnaligned16 ((CONST UINT16 *)(List + Index * sizeof (UINT16)));
    if (Gpio == DWC3_HOST_GPIO_LIST_END) {
      break;
    }

    if (Gpio >= TLMM_GPIO_COUNT) {
      DEBUG ((DEBUG_ERROR, "%a: no GPIO%u\n", __func__, Gpio));
      return EFI_INVALID_PARAMETER;
    }

    if (TlmmIsDrivenHigh (Gpio)) {
      DEBUG ((
        DEBUG_INFO,
        "%a: GPIO%u driven high already (CFG 0x%x, IO 0x%x)\n",
        __func__,
        Gpio,
        MmioRead32 (TLMM_GPIO_CFG (Gpio)),
        MmioRead32 (TLMM_GPIO_IO (Gpio))
        ));
      continue;
    }

    //
    // The library logs the pin before and after.
    //
    Status = TlmmDriveOutput (Gpio, TRUE, DWC3_HOST_GPIO_DRIVE_MA);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: GPIO%u: %r\n", __func__, Gpio, Status));
      return Status;
    }

    Changed = TRUE;
  }

  if (Changed) {
    MicroSecondDelay (DWC3_HOST_GPIO_RAMP_US);
  }

  return EFI_SUCCESS;
}

/**
  Votes a PMIC5 LDO on, at a voltage, in high power mode. Like
  qcom-rpmh-regulator.c (and MdssDisplayDxe), each setting goes as its own
  request and waits for its acknowledgement.

  @param[in]  Rail  The LDO.

  @retval EFI_SUCCESS  Voted.
  @retval Other        From RpmhWrite().
**/
STATIC
EFI_STATUS
Dwc3HostVoteRail (
  IN CONST DWC3_HOST_RAIL  *Rail
  )
{
  RPMH_CMD    Cmd;
  EFI_STATUS  Status;

  Cmd.Wait = TRUE;

  Cmd.Addr = Rail->Addr + RPMH_VRM_VOLTAGE;
  Cmd.Data = Rail->MilliVolts;
  Status   = RpmhWrite (&Cmd, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd.Addr = Rail->Addr + RPMH_VRM_MODE;
  Cmd.Data = RPMH_PMIC5_LDO_MODE_HPM;
  Status   = RpmhWrite (&Cmd, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd.Addr = Rail->Addr + RPMH_VRM_ENABLE;
  Cmd.Data = 1;
  return RpmhWrite (&Cmd, 1);
}

/**
  Turns the HS PHY supplies on through RPMh.

  @retval EFI_SUCCESS  On.
  @retval Other        RPMh is not usable or a vote failed.
**/
STATIC
EFI_STATUS
Dwc3HostRailsOn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  Status = RpmhInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: RPMh not usable: %r\n", __func__, Status));
    return Status;
  }

  for (Index = 0; Index < ARRAY_SIZE (mRails); Index++) {
    Status = Dwc3HostVoteRail (&mRails[Index]);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: %a (0x%05x) at %u mV: %r\n",
        __func__,
        mRails[Index].Name,
        mRails[Index].Addr,
        mRails[Index].MilliVolts,
        Status
        ));
      return Status;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: %a (0x%05x) on at %u mV, HPM\n",
      __func__,
      mRails[Index].Name,
      mRails[Index].Addr,
      mRails[Index].MilliVolts
      ));
  }

  MicroSecondDelay (DWC3_HOST_RAIL_SETTLE_US);
  return EFI_SUCCESS;
}

/**
  Makes sure GPLL0, the source the master root moves to, runs: in FSM vote
  mode it is voted on for the apps processor and its ACTIVE flag awaited,
  as clk_trion_pll_enable does. A PLL under manual control is left to the
  boot firmware that set it up.

  @retval EFI_SUCCESS  GPLL0 runs, or is not under vote control.
  @retval EFI_TIMEOUT  It did not become active.
**/
STATIC
EFI_STATUS
Dwc3HostGpll0On (
  VOID
  )
{
  UINT32      Mode;
  UINT32      Vote;
  EFI_STATUS  Status;

  Mode = MmioRead32 (GCC_GPLL0_MODE);
  Vote = MmioRead32 (GCC_GPLL0_ENA_VOTE);

  if ((Mode & GCC_GPLL0_MODE_VOTE_FSM_ENA) == 0) {
    DEBUG ((DEBUG_INFO, "%a: GPLL0 under manual control (mode 0x%08x), left as is\n", __func__, Mode));
    return EFI_SUCCESS;
  }

  if ((Vote & GCC_GPLL0_ENA) == 0) {
    MmioWrite32 (GCC_GPLL0_ENA_VOTE, Vote | GCC_GPLL0_ENA);
  }

  Status = GccPoll32 (GCC_GPLL0_MODE, GCC_GPLL0_MODE_ACTIVE_FLAG, GCC_GPLL0_MODE_ACTIVE_FLAG, GCC_GPLL0_ACTIVE_TIMEOUT_US);
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: GPLL0 %a: vote 0x%x -> 0x%x, mode 0x%08x\n",
    __func__,
    EFI_ERROR (Status) ? "did not become active" : "active",
    Vote,
    MmioRead32 (GCC_GPLL0_ENA_VOTE),
    MmioRead32 (GCC_GPLL0_MODE)
    ));

  return Status;
}

/**
  Sets the usb_2 clock rates, powers its domain, resets the controller and
  turns its clocks on, in the order Linux does (of_clk_set_defaults,
  dev_pm_domain_attach, then dwc3_qcom_probe).

  @retval EFI_SUCCESS  The controller is powered, out of reset and clocked.
  @retval Other        A root, the GDSC or a branch did not respond.
**/
STATIC
EFI_STATUS
Dwc3HostClocksOn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      MockCfg;
  UINTN       Index;

  Status = Dwc3HostGpll0On ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Master root: 120 MHz, the fastest entry of its table, which is what
  // Linux sets for the DT's 200 MHz (qcom_find_freq). The library logs each
  // step of this function.
  //
  Status = GccRcgConfigure (GCC_USB30_SEC_MASTER_CMD_RCGR, USB30_SEC_MASTER_CFG_120MHZ, "gcc_usb30_sec_master_clk_src");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Mock UTMI root: 19.2 MHz from XO. Linux finds it at that rate and does
  // not write it; the boot firmware leaves it on XO with SRC_DIV 0 and
  // HW_CLK_CTRL set. Only a root found elsewhere is moved to XO / 1. The
  // post-divider after it is read-only for Linux too.
  //
  MockCfg = MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR + RCG_CFG_OFFSET);
  if ((((MockCfg & RCG_CFG_SRC_SEL_MASK) >> RCG_CFG_SRC_SEL_SHIFT) == USB30_SEC_MOCK_UTMI_SRC_XO) &&
      ((MockCfg & RCG_CFG_SRC_DIV_MASK) <= 1))
  {
    DEBUG ((DEBUG_INFO, "%a: mock UTMI root on XO undivided (cfg 0x%08x), left as is\n", __func__, MockCfg));
  } else {
    Status = GccRcgConfigure (GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR, USB30_SEC_MOCK_UTMI_CFG_19_2MHZ, "gcc_usb30_sec_mock_utmi_clk_src");
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  DEBUG ((
    ((MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_POSTDIV) & 0xF) == 0) ? DEBUG_INFO : DEBUG_WARN,
    "%a: master cfg 0x%08x, mock UTMI cfg 0x%08x postdiv 0x%x (0 = /1)\n",
    __func__,
    MmioRead32 (GCC_USB30_SEC_MASTER_CMD_RCGR + RCG_CFG_OFFSET),
    MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_CMD_RCGR + RCG_CFG_OFFSET),
    MmioRead32 (GCC_USB30_SEC_MOCK_UTMI_POSTDIV)
    ));

  //
  // The power domain, with gdsc.c's default wait times (gcc_usb30_sec_gdsc
  // sets none of its own).
  //
  Status = GccGdscEnable (GCC_USB30_SEC_GDSCR, GCC_GDSC_WAITS (2, 8, 2), "gcc_usb30_sec_gdsc");
  if (EFI_ERROR (Status)) {
    GccLogUsb2 ();
    return Status;
  }

  //
  // dwc3-qcom.c resets the controller with its domain on and before it
  // turns the clocks on.
  //
  GccResetPulse (GCC_USB30_SEC_BCR, USB30_SEC_BCR_ASSERT_US, USB30_SEC_BCR_DEASSERT_US);

  for (Index = 0; Index < ARRAY_SIZE (mBranches); Index++) {
    Status = GccBranchEnable (
               mBranches[Index].Cbcr,
               CBCR_CLK_ENABLE,
               mBranches[Index].Cbcr,
               mBranches[Index].Halt,
               mBranches[Index].Name
               );
    if (EFI_ERROR (Status)) {
      GccLogUsb2 ();
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Identifies the core (dwc3_core_is_valid, dwc3_cache_hwparams).

  @param[out]  Revision   The core revision, e.g. 0x330A for 3.30a.
  @param[out]  HwParams0  GHWPARAMS0.
  @param[out]  HwParams1  GHWPARAMS1.

  @retval EFI_SUCCESS       A DWC_usb3 core that can be a host.
  @retval EFI_UNSUPPORTED   Something else answers.
**/
STATIC
EFI_STATUS
Dwc3CoreIdentify (
  OUT UINT32  *Revision,
  OUT UINT32  *HwParams0,
  OUT UINT32  *HwParams1
  )
{
  UINT32  Id;
  UINT32  HwParams3;
  UINT32  HwParams6;

  Id = MmioRead32 (USB2_DWC3_BASE + DWC3_GSNPSID);
  if ((Id & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_DWC_USB3) {
    DEBUG ((DEBUG_ERROR, "%a: GSNPSID 0x%08x is not a DWC_usb3 core\n", __func__, Id));
    GccLogUsb2 ();
    return EFI_UNSUPPORTED;
  }

  *Revision  = Id & DWC3_GSNPSREV_MASK;
  *HwParams0 = MmioRead32 (USB2_DWC3_BASE + DWC3_GHWPARAMS0);
  *HwParams1 = MmioRead32 (USB2_DWC3_BASE + DWC3_GHWPARAMS1);
  HwParams3  = MmioRead32 (USB2_DWC3_BASE + DWC3_GHWPARAMS3);
  HwParams6  = MmioRead32 (USB2_DWC3_BASE + DWC3_GHWPARAMS6);

  DEBUG ((
    DEBUG_INFO,
    "%a: GSNPSID 0x%08x (DWC_usb3 rev %x), GHWPARAMS0 0x%08x 1 0x%08x 3 0x%08x 6 0x%08x: mode %u, PWROPT %u, SSPHY_IFC %u, HSPHY_IFC %u\n",
    __func__,
    Id,
    *Revision,
    *HwParams0,
    *HwParams1,
    HwParams3,
    HwParams6,
    *HwParams0 & DWC3_GHWPARAMS0_MODE_MASK,
    (*HwParams1 >> DWC3_GHWPARAMS1_EN_PWROPT_SHIFT) & DWC3_GHWPARAMS1_EN_PWROPT_MASK,
    HwParams3 & DWC3_GHWPARAMS3_SSPHY_IFC_MASK,
    (HwParams3 >> DWC3_GHWPARAMS3_HSPHY_IFC_SHIFT) & DWC3_GHWPARAMS3_HSPHY_IFC_MASK
    ));

  if ((*HwParams0 & DWC3_GHWPARAMS0_MODE_MASK) == DWC3_GHWPARAMS0_MODE_GADGET) {
    DEBUG ((DEBUG_ERROR, "%a: a device-only core cannot be a host\n", __func__));
    return EFI_UNSUPPORTED;
  }

  Dwc3Log (DEBUG_INFO, "reset");
  return EFI_SUCCESS;
}

/**
  Initialises the HS PHY as qcom_snps_hsphy_init does once its supplies
  and reference clock (XO) are on: resets it, runs the init sequence and
  checks that the bits it wrote read back.

  @retval EFI_SUCCESS       Initialised.
  @retval EFI_DEVICE_ERROR  A written bit did not read back.
**/
STATIC
EFI_STATUS
HsPhyInit (
  VOID
  )
{
  UINTN    Index;
  UINTN    Write;
  UINTN    Address;
  UINT32   Reg;
  UINT32   Mask;
  UINT32   Value;
  UINT8    Byte;
  BOOLEAN  Failed;

  //
  // The PHY sits behind the AHB2PHY bridge, which has no clock Linux
  // manages: a hang here would point at it.
  //
  HsPhyLog ("entry");

  GccResetPulse (GCC_QUSB2PHY_SEC_BCR, QUSB2PHY_SEC_BCR_ASSERT_US, QUSB2PHY_SEC_BCR_DEASSERT_US);

  HsPhyLog ("reset");

  for (Index = 0; Index < ARRAY_SIZE (mHsPhyInit); Index++) {
    Address = USB2_HS_PHY_BASE + mHsPhyInit[Index].Offset;
    Reg     = MmioRead32 (Address);
    Reg    &= ~mHsPhyInit[Index].Mask;
    Reg    |= mHsPhyInit[Index].Value & mHsPhyInit[Index].Mask;
    MmioWrite32 (Address, Reg);
    MmioRead32 (Address);   // the write has landed before the next one
  }

  HsPhyLog ("init");

  //
  // For each register, the bits the sequence wrote and their last value
  // must read back; the rest is compared with what Linux leaves.
  //
  Failed = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mHsPhyExpected); Index++) {
    Mask  = 0;
    Value = 0;
    for (Write = 0; Write < ARRAY_SIZE (mHsPhyInit); Write++) {
      if (mHsPhyInit[Write].Offset == mHsPhyExpected[Index].Offset) {
        Mask |= mHsPhyInit[Write].Mask;
        Value = (Value & ~mHsPhyInit[Write].Mask) | (mHsPhyInit[Write].Value & mHsPhyInit[Write].Mask);
      }
    }

    Byte = (UINT8)MmioRead32 (USB2_HS_PHY_BASE + mHsPhyExpected[Index].Offset);
    if ((Byte & Mask) != Value) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: 0x%02x reads 0x%02x, the bits 0x%02x should read 0x%02x\n",
        __func__,
        mHsPhyExpected[Index].Offset,
        Byte,
        Mask,
        Value
        ));
      Failed = TRUE;
    } else if (Byte != mHsPhyExpected[Index].Expected) {
      DEBUG ((
        DEBUG_WARN,
        "%a: 0x%02x reads 0x%02x, Linux leaves 0x%02x (reset value bits differ)\n",
        __func__,
        mHsPhyExpected[Index].Offset,
        Byte,
        mHsPhyExpected[Index].Expected
        ));
    }
  }

  if (Failed) {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "%a: HS PHY initialised\n", __func__));
  return EFI_SUCCESS;
}

/**
  dwc3_core_init up to the event buffers, which only device mode uses:
  PHY interface set-up, the HS PHY init, the device-side soft reset and the
  global control registers.

  @param[in]  Revision   The core revision.
  @param[in]  HwParams1  GHWPARAMS1.

  @retval EFI_SUCCESS       Done.
  @retval EFI_DEVICE_ERROR  The PHY or the soft reset failed.
**/
STATIC
EFI_STATUS
Dwc3CoreInit (
  IN UINT32  Revision,
  IN UINT32  HwParams1
  )
{
  EFI_STATUS  Status;
  UINT32      Reg;
  UINT32      PowerOpt;

  //
  // dwc3_phy_setup: UX_EXIT_PX off, and the PHYs kept out of suspend while
  // they initialise. ENBLSLPM off for snps,dis_enblslpm_quirk. No other
  // quirk applies to usb_2, and with no phy_type in the DT the UTMI width
  // and turnaround time stay at their reset values.
  //
  MmioAnd32 (
    USB2_DWC3_BASE + DWC3_GUSB3PIPECTL0,
    (UINT32) ~(DWC3_GUSB3PIPECTL_UX_EXIT_PX | DWC3_GUSB3PIPECTL_SUSPHY)
    );
  MmioAnd32 (
    USB2_DWC3_BASE + DWC3_GUSB2PHYCFG0,
    (UINT32) ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM)
    );

  Status = HsPhyInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // dwc3_phy_init: past 1.94a the PHYs may be suspended once initialised.
  // Only the USB3 one: snps,dis_u2_susphy_quirk keeps the HS PHY awake.
  //
  if (Revision > DWC3_REVISION_194A) {
    MmioOr32 (USB2_DWC3_BASE + DWC3_GUSB3PIPECTL0, DWC3_GUSB3PIPECTL_SUSPHY);
  }

  //
  // dwc3_core_soft_reset (no role yet, so it runs): CSFTRST set, RUN_STOP
  // clear, no link state change request (dwc3_gadget_dctl_write_safe).
  // The bit clears once the reset has gone through the core's clock
  // domains, the PHY's UTMI clock among them (INFERRED), so a timeout
  // usually points at the PHY or a clock.
  //
  Reg  = MmioRead32 (USB2_DWC3_BASE + DWC3_DCTL);
  Reg |= DWC3_DCTL_CSFTRST;
  Reg &= ~(UINT32)(DWC3_DCTL_RUN_STOP | DWC3_DCTL_ULSTCHNGREQ_MASK);
  MmioWrite32 (USB2_DWC3_BASE + DWC3_DCTL, Reg);

  Status = GccPoll32 (USB2_DWC3_BASE + DWC3_DCTL, DWC3_DCTL_CSFTRST, 0, DWC3_CSFTRST_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: core soft reset did not complete\n", __func__));
    Dwc3Log (DEBUG_ERROR, "soft reset");
    HsPhyLog ("soft reset");
    GccLogUsb2 ();
    return EFI_DEVICE_ERROR;
  }

  //
  // dwc3_core_setup_global_control: no scale-down; clock gating allowed
  // unless the revision has the 2.10a-2.50a SOF/ITP erratum; scrambling
  // on (not an FPGA, and no disable_scramble quirk); U2RSTECN before 1.90a.
  //
  Reg      = MmioRead32 (USB2_DWC3_BASE + DWC3_GCTL);
  Reg     &= ~(UINT32)DWC3_GCTL_SCALEDOWN_MASK;
  PowerOpt = (HwParams1 >> DWC3_GHWPARAMS1_EN_PWROPT_SHIFT) & DWC3_GHWPARAMS1_EN_PWROPT_MASK;
  if (PowerOpt == DWC3_GHWPARAMS1_EN_PWROPT_CLK) {
    if ((Revision >= DWC3_REVISION_210A) && (Revision <= DWC3_REVISION_250A)) {
      Reg |= DWC3_GCTL_DSBLCLKGTNG | DWC3_GCTL_SOFITPSYNC;
    } else {
      Reg &= ~(UINT32)DWC3_GCTL_DSBLCLKGTNG;
    }
  } else if (PowerOpt == DWC3_GHWPARAMS1_EN_PWROPT_HIB) {
    Reg |= DWC3_GCTL_GBLHIBERNATIONEN;
  }

  Reg &= ~(UINT32)DWC3_GCTL_DISSCRAMBLE;
  if (Revision < DWC3_REVISION_190A) {
    Reg |= DWC3_GCTL_U2RSTECN;
  }

  MmioWrite32 (USB2_DWC3_BASE + DWC3_GCTL, Reg);

  //
  // dwc3_core_init after the event buffers: GUCTL2.RST_ACTBITLATER from
  // 3.10a (and LC_TIMER off on 3.20a), then the GUCTL1 bits from 2.50a.
  // The GUCTL1 bits are device-side; they are set as Linux sets them, with
  // DEV_FORCE_20_CLK_FOR_30_CLK for maximum-speed = "high-speed".
  //
  if (Revision >= DWC3_REVISION_310A) {
    MmioOr32 (USB2_DWC3_BASE + DWC3_GUCTL2, DWC3_GUCTL2_RST_ACTBITLATER);
  }

  if (Revision == DWC3_REVISION_320A) {
    MmioAnd32 (USB2_DWC3_BASE + DWC3_GUCTL2, (UINT32) ~DWC3_GUCTL2_LC_TIMER);
  }

  if (Revision >= DWC3_REVISION_250A) {
    Reg = MmioRead32 (USB2_DWC3_BASE + DWC3_GUCTL1);
    if (Revision >= DWC3_REVISION_290A) {
      Reg |= DWC3_GUCTL1_DEV_L1_EXIT_BY_HW | DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;
    }

    if (Revision >= DWC3_REVISION_300A) {
      Reg |= DWC3_GUCTL1_DEV_DECOUPLE_L1L2_EVT;
    }

    MmioWrite32 (USB2_DWC3_BASE + DWC3_GUCTL1, Reg);
  }

  Dwc3Log (DEBUG_INFO, "core init");
  return EFI_SUCCESS;
}

/**
  Switches the core to host mode (dwc3_set_prtcap) and checks that the
  xHCI register file answers.

  The PHY suspend bits stay off from here: a dual-role core has them off
  while it switches, and Linux sets the USB3 one again only once the xHCI
  has been reset and started (dwc3_xhci_plat_start), which XhciDxe does
  here. usb_2 has no USB3 PHY, so that bit gates nothing.

  @param[in]  HwParams0  GHWPARAMS0.

  @retval EFI_SUCCESS       Host mode, xHCI present.
  @retval EFI_DEVICE_ERROR  The xHCI capability registers make no sense.
**/
STATIC
EFI_STATUS
Dwc3SetHostMode (
  IN UINT32  HwParams0
  )
{
  UINT32  Reg;
  UINT32  Cap;
  UINT32  CapLength;
  UINT32  HciVersion;
  UINT32  HcsParams1;
  UINT32  HccParams1;

  if ((HwParams0 & DWC3_GHWPARAMS0_MODE_MASK) == DWC3_GHWPARAMS0_MODE_DRD) {
    MmioAnd32 (USB2_DWC3_BASE + DWC3_GUSB3PIPECTL0, (UINT32) ~DWC3_GUSB3PIPECTL_SUSPHY);
    MmioAnd32 (USB2_DWC3_BASE + DWC3_GUSB2PHYCFG0, (UINT32) ~DWC3_GUSB2PHYCFG_SUSPHY);
  }

  Reg  = MmioRead32 (USB2_DWC3_BASE + DWC3_GCTL);
  Reg &= ~(UINT32)DWC3_GCTL_PRTCAPDIR_MASK;
  Reg |= DWC3_GCTL_PRTCAP_HOST << DWC3_GCTL_PRTCAPDIR_SHIFT;
  MmioWrite32 (USB2_DWC3_BASE + DWC3_GCTL, Reg);

  Dwc3Log (DEBUG_INFO, "host");

  Cap        = MmioRead32 (USB2_DWC3_BASE + XHC_CAPLENGTH_HCIVERSION);
  CapLength  = Cap & 0xFF;
  HciVersion = Cap >> 16;
  HcsParams1 = MmioRead32 (USB2_DWC3_BASE + XHC_HCSPARAMS1);
  HccParams1 = MmioRead32 (USB2_DWC3_BASE + XHC_HCCPARAMS1);

  if ((CapLength < XHC_CAPLENGTH_MIN) || (HciVersion < XHC_HCIVERSION_MIN) ||
      (XHC_HCSPARAMS1_MAX_PORTS (HcsParams1) == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no xHCI: CAPLENGTH/HCIVERSION 0x%08x HCSPARAMS1 0x%08x HCCPARAMS1 0x%08x\n",
      __func__,
      Cap,
      HcsParams1,
      HccParams1
      ));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: xHCI %x.%02x, CAPLENGTH 0x%x, %u port(s), AC64 %u, PPC %u; USBCMD 0x%x USBSTS 0x%x PORTSC1 0x%08x; GSTS.CURMOD %u\n",
    __func__,
    HciVersion >> 8,
    HciVersion & 0xFF,
    CapLength,
    XHC_HCSPARAMS1_MAX_PORTS (HcsParams1),
    (HccParams1 & XHC_HCCPARAMS1_AC64) != 0,
    (HccParams1 & XHC_HCCPARAMS1_PPC) != 0,
    MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_USBCMD),
    MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_USBSTS),
    MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_PORTSC1),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GSTS) & DWC3_GSTS_CURMOD_MASK
    ));

  return EFI_SUCCESS;
}

/**
  The xHCI's PCI I/O Attributes() without dual address cycles: the
  attribute is neither reported as supported nor enabled, so XhciDxe keeps
  its DMA below 4 GiB (see the file header).

  @param[in]   This       The PCI I/O.
  @param[in]   Operation  What to do.
  @param[in]   Attributes The attributes.
  @param[out]  Result     The attributes read.

  @retval EFI_UNSUPPORTED  Dual address cycles were asked for.
  @retval Other            From the PCI I/O's Attributes().
**/
STATIC
EFI_STATUS
EFIAPI
Dwc3HostPciIoAttributes (
  IN  EFI_PCI_IO_PROTOCOL                       *This,
  IN  EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION  Operation,
  IN  UINT64                                    Attributes,
  OUT UINT64                                    *Result OPTIONAL
  )
{
  EFI_STATUS  Status;

  if (((Operation == EfiPciIoAttributeOperationEnable) || (Operation == EfiPciIoAttributeOperationSet)) &&
      ((Attributes & EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE) != 0))
  {
    if (Operation == EfiPciIoAttributeOperationEnable) {
      DEBUG ((DEBUG_INFO, "%a: no dual address cycles: the xHCI's DMA stays below 4 GiB\n", __func__));
      return EFI_UNSUPPORTED;
    }

    Attributes &= ~(UINT64)EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  }

  Status = mXhciPciIoAttributes (This, Operation, Attributes, Result);
  if (!EFI_ERROR (Status) && (Result != NULL) &&
      ((Operation == EfiPciIoAttributeOperationSupported) || (Operation == EfiPciIoAttributeOperationGet)))
  {
    *Result &= ~(UINT64)EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  }

  return Status;
}

/**
  Filters the xHCI's PCI I/O as soon as NonDiscoverablePciDeviceDxe has
  installed it on the handle the xHCI was registered on, before XhciDxe
  starts on it (the DXE core runs this notification when the installation
  returns, and connects the next driver after).

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
Dwc3HostPciIoNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_PCI_IO_PROTOCOL  *PciIo;

  if ((mXhciPciIo != NULL) ||
      EFI_ERROR (gBS->HandleProtocol (mXhciHandle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo)))
  {
    return;
  }

  mXhciPciIo           = PciIo;
  mXhciPciIoAttributes = PciIo->Attributes;
  PciIo->Attributes    = Dwc3HostPciIoAttributes;
  gBS->CloseEvent (Event);

  DEBUG ((DEBUG_INFO, "%a: xHCI PCI I/O %p: DMA kept below 4 GiB\n", __func__, PciIo));
}

/**
  Logs a host system error, a host controller error or an AXI bus error of
  the controller, each kind once, with the state of the controller and its
  port and the address of the failed bus transfer the core recorded.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
Dwc3HostWatch (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  CapLength;
  UINT32  UsbSts;
  UINT32  Gsts;
  UINT32  Errors;

  CapLength = MmioRead32 (USB2_DWC3_BASE + XHC_CAPLENGTH_HCIVERSION) & 0xFF;
  UsbSts    = MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_USBSTS);
  Gsts      = MmioRead32 (USB2_DWC3_BASE + DWC3_GSTS);
  Errors    = (UsbSts & (XHC_USBSTS_HSE | XHC_USBSTS_HCE)) |
              (((Gsts & DWC3_GSTS_BUS_ERR_ADDR_VLD) != 0) ? BIT31 : 0);

  if ((Errors & ~mWatchReported) == 0) {
    return;
  }

  mWatchReported |= Errors;
  DEBUG ((
    DEBUG_ERROR,
    "%a: usb_2 xHCI error: USBSTS 0x%x (HSE %u HCE %u HCH %u) USBCMD 0x%x PORTSC1 0x%08x, GSTS 0x%08x, bus error at 0x%08x%08x\n",
    __func__,
    UsbSts,
    (UsbSts & XHC_USBSTS_HSE) != 0,
    (UsbSts & XHC_USBSTS_HCE) != 0,
    (UsbSts & XHC_USBSTS_HCH) != 0,
    MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_USBCMD),
    MmioRead32 (USB2_DWC3_BASE + CapLength + XHC_OP_PORTSC1),
    Gsts,
    MmioRead32 (USB2_DWC3_BASE + DWC3_GBUSERRADDR1),
    MmioRead32 (USB2_DWC3_BASE + DWC3_GBUSERRADDR0)
    ));
}

/**
  Stops checking the controller at ExitBootServices: the OS drives it then.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
Dwc3HostExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->SetTimer (mWatchEvent, TimerCancel, 0);
}

/**
  Sets up what goes with the registered xHCI: the PCI I/O filter and the
  error check.
**/
STATIC
VOID
Dwc3HostWatchXhci (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Dwc3HostPciIoNotify, NULL, &mPciIoEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->RegisterProtocolNotify (&gEfiPciIoProtocolGuid, mPciIoEvent, &mPciIoRegistration);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no PCI I/O notification (%r): the xHCI's DMA may go above 4 GiB\n", __func__, Status));
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Dwc3HostWatch, NULL, &mWatchEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mWatchEvent, TimerPeriodic, DWC3_HOST_WATCH_PERIOD);
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    Dwc3HostExitBootServices,
                    NULL,
                    &gEfiEventExitBootServicesGuid,
                    &mExitBootServicesEvent
                    );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no error check: %r\n", __func__, Status));
  }
}

/**
  Entry point: brings usb_2 up in host mode and registers its xHCI.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS      The xHCI is registered.
  @retval EFI_UNSUPPORTED  The board does not use usb_2 as a host.
  @retval Other            Bring-up failed; nothing was registered.
**/
EFI_STATUS
EFIAPI
Dwc3HostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Revision;
  UINT32      HwParams0;
  UINT32      HwParams1;

  if (!PcdGetBool (PcdUsbSecondaryHostEnable)) {
    return EFI_UNSUPPORTED;
  }

  DEBUG ((DEBUG_INFO, "%a: usb_2 at 0x%08x, host mode\n", __func__, USB2_DWC3_BASE));

  Status = Dwc3HostPowerGpios ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Dwc3HostRailsOn ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Dwc3HostClocksOn ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Dwc3CoreIdentify (&Revision, &HwParams0, &HwParams1);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Dwc3CoreInit (Revision, HwParams1);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Dwc3SetHostMode (HwParams0);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // The xHCI window only: XhciDxe has no business with the DWC3 globals or
  // QSCRATCH. The base and size are passed as UINTN, the type the library
  // takes them as from the variable arguments.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             &mXhciHandle,
             1,
             (UINTN)USB2_DWC3_BASE,
             (UINTN)USB2_XHCI_SIZE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: registering the xHCI failed: %r\n", __func__, Status));
    goto Fail;
  }

  Dwc3HostWatchXhci ();

  DEBUG ((
    DEBUG_INFO,
    "%a: xHCI 0x%08x+0x%x registered (non-coherent DMA below 4 GiB)\n",
    __func__,
    USB2_DWC3_BASE,
    USB2_XHCI_SIZE
    ));
  return EFI_SUCCESS;

Fail:
  DEBUG ((DEBUG_ERROR, "%a: usb_2 not available: %r\n", __func__, Status));
  return Status;
}
