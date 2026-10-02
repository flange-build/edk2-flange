# RUBIK Pi 3: peripheral inventory for Windows 11 on Arm

Task: board-hw. This lists every peripheral of the Thundercomm RUBIK Pi 3 (QCS6490, the SC7280 "kodiak" die).
For each one it gives the SoC block or bus, the chip, what Windows needs to drive it, and how likely that is.
Peripherals are ranked by what a usable install needs: storage, display, keyboard/mouse and network.

Conventions:
- **INFERRED** marks a conclusion not checked on the board or in a binary.
- Line numbers point into these local files:
  - `V:` the vendor 6.6 DTS, `<vendor-kernel-6.6>/arch/arm64/boot/dts/qcom/qcs6490-thundercomm-rubikpi3.dtsi`
  - `M:` the mainline board DTS, `devicetree/mainline/upstream/src/arm64/qcom/qcs6490-thundercomm-rubikpi3.dts`. It is identical to Linux 7.0.2 except for the LT9611 port: port@1 here, port@0 in 7.0.2.
  - `K:` `kodiak.dtsi`, in the same mainline directory.
  - `E:` `edk2-qualcomm/Silicon/Qualcomm/QCS6490/`
- Windows inbox INF files were extracted from the local Win11 25H2 ARM64 ISO into the session scratchpad. They are not in the repo.
  - `WinPE:` comes from `sources/boot.wim` index 2.
  - `OS:` comes from `sources/install.wim` index 1.
  - Their line numbers refer to the UTF-8 converted INF.
- `Q8B:` means `<downloads>/dragon-q8b_win_driver_pack_v1.0.0/driver_pack/` (SC8280XP, "8280" drivers).
  - The pack is re-signed with "Radxa WoS Test Cert": the `.cat` signer, checked with `openssl pkcs7`.
- Likelihood scale:
  - **High**: works with inbox drivers once the firmware is right.
  - **Med**: needs Qualcomm or third-party drivers that exist, plus ACPI work.
  - **Low**: needs a driver that has to be written or ported.
  - **None**: no realistic path.

---

## 1. Key findings

1. **Qualcomm-architecture Windows drivers exist for this SoC.**
   - Microsoft lists QCS6490/QCM6490 and Snapdragon 7c+ Gen 3 as supported for Win11 IoT Enterprise LTSC 2024, 24H2 and 25H2 ([MS Learn][mslist]).
   - Production-signed SC7280 drivers ("7280" suffix, from Windows Update) are mirrored by WOA-Project: `7280_WINDOWS_CLS/200.0.15.0`, 2024-07-08, `qcdx7280` 30.0.4056.4400 ([repo][woarepo], [changelog][woachg]).
   - Radxa ships a **QCS6490** Windows driver pack for the Dragon Q6A, `Q6A_WoS_DriverPackage_251205_testsigned.7z`, together with a Windows-capable UEFI ([Radxa docs][radxawin], [CNX][cnxq6a]).
   - With it, HDMI (through DP), GPU (D3D12 FL12_1), VPU, camera (IMX577), audio (WCD9380), Ethernet (PCIe RTL8111K), USB, UFS/eMMC/NVMe and 40-pin GPIO work.
   - The Q6A UEFI in the local flat build (251013) has **no** ACPI. Its module list in `windows-research/q6a-fw/out/inventory.tsv` has no AcpiPlatform and no tables.
   - A Thundercomm forum user reported a Windows proof of concept on the RUBIK Pi 3, built with the Qualcomm "Kodiak LA/LE/WP BSP".
   - Thundercomm said it would open-source the "LT9611 MDPPlatformLib" code in March 2026 ([forum][rubikforum]).
2. **The inbox WinPE drivers already cover the boot-critical blocks** of a Qualcomm SoC, if the firmware leaves the hardware initialized:

   | Driver | Binds to | Covers | Source |
   |---|---|---|---|
   | `storufs` | `ACPI\QCOM24A5` | UFS | WinPE storufs.inf:84; install section :352-360 |
   | `usbxhci` | `ACPI\PNP0D10`, `PCI\CC_0C0330` | DWC3 in host mode, Renesas xHCI | usbxhci.inf:38-39 |
   | `urssynopsys` | `ACPI\QCOM24B6` | DWC3 dual-role | urssynopsys.inf:34 |
   | `sdbus` | `ACPI\QCOM24BF` | SDHCI | sdbus.inf:175 |
   | `netax88179_178a` | `USB\VID_0B95&PID_1790` | the on-board AX88179B | netax88179_178a.inf:21 |
   | `qcgpio_i`, `qci2c_i`, `QcTrEE_i` | `QCOMFFEB`, `QCOMFFEA`, `QCOMFFEC` | Qualcomm TLMM, I2C, TrustZone | qcgpio_i.inf:43, qci2c_i.inf:41, QcTrEE_i.inf:38 |

   The full OS adds:

   | Driver | Binds to | Covers | Source |
   |---|---|---|---|
   | `usbncm` | `USB\MS_COMP_WINNCM`, CDC-NCM class | the AX88179B in NCM mode | usbncm.inf:27 |
   | `rhproxy` | `ACPI\MSFT8000` | user-mode GPIO/I2C/SPI | rhproxy.inf:35 |
   | MPTF | `ACPI\MSFT000D` | Platform Thermal Framework | MptfCore.inf:42 |
   | `MSTemperatureSensor` | `ACPI\MSFT000A` | ACPI temperature sensors | MSTemperatureSensor.inf:42 |
   | `usbaudio2`, `usbvideo` | USB classes | USB audio and webcams | - |

3. **A usable install, the minimum set:**
   - Storage: UFS through `storufs`.
   - Display: HDMI as a BasicDisplay (GOP) framebuffer.
   - Input: USB HID on `usb_2` through `PNP0D10`.
   - Network: the AX88179B behind the Renesas xHCI on PCIe0 (inbox driver), or a USB Ethernet dongle on `usb_2`. The dongle is the fallback that avoids the PCIe dependency.
   - All of these are inbox. None needs Qualcomm drivers or PEP, if the UEFI hands over hardware that is powered, clocked and in SMMU bypass.
4. **HDMI is the hardest of the four.** The path is DSI0, then a non-UXC Lontium LT9611 that the host must program over I2C.
   - Qualcomm's own display stack handles DSI-HDMI bridges as a fixed-timing "panel". The Qualcomm UEFI ships `Panel_lt9611uxc_dsi2hdmi_vid.xml` (1080p, DSI 4-lane, reset GPIO 111) for the RB3 Gen 2's LT9611UXC (see §4.1).
   - The plain LT9611 needs an extra component: Thundercomm's MDPPlatformLib on UEFI, and under Windows **INFERRED** UEFI pre-programming plus re-locking the bridge after `qcdx` restarts DSI.
   - Plan BasicDisplay first. It needs two changes:
     - `MdssDisplayDxe` must stop tearing the display down at BeforeExitBootServices (E:`Drivers/MdssDisplayDxe/MdssDisplayDxe.c:9-11,141-158`).
     - The MDSS SMMU stream must stay in bypass: run at EL2, or keep the Gunyah bypass entry.
     The framebuffer is already `EfiReservedMemoryType` (MdssDisplayDxe.c:191-193).
5. **Wi-Fi has no path.** The on-board AP6256 (BCM43456 rev 9 Wi-Fi over SDIO, BCM4345C5 Bluetooth over UART) has no Windows ARM64 Broadcom SDIO WLAN driver. Even the Raspberry Pi WoR project has none for CYW43455 ([WoR][wordrv]).
   - Bluetooth: Low-Med. The only ARM64 candidate is Cypress's `cywbtserialbus.sys` from WoR, used for the RPi 4's CYW43455 UART Bluetooth.
6. **Audio jack has no off-the-shelf path.** The ES8316 hangs off LPASS primary MI2S, and Qualcomm's Windows audio stack expects WCD938x over SoundWire through the ADSP.
   - A custom LPAIF-MI2S + ES8316 driver, modelled on CoolStar's open `csaudiork3x` + `es8323` from the local Rockchip pack, is possible (**INFERRED**) but large. USB audio is inbox.
7. **Platform gaps in today's UEFI that Windows will hit:**
   - The RTC is `VirtualRealTimeClockLib` (E:`QCS6490.dsc.inc:136`), so Windows reads a fake time.
   - Runtime SetVariable is not persisted. Windows Setup writes Boot#### / BootOrder.
   - CPU DVFS, DDR bandwidth and thermal are all PEP-managed on Qualcomm Windows. Without PEP the system runs at the firmware's frequencies and votes. Do not expose `_LPI` domain states 0x41000044/0x41001344/0x4100b344 (K:481-501): an apps-RSC sleep would drop the ACTIVE_ONLY RPMh votes the UEFI makes (E:`Library/Qcs6490RpmhLib/Rpmh.c:1-12`). **INFERRED**.
   - The fan is not driven by anything in Windows. Set a fixed PWM in UEFI.

---

## 2. Ranked inventory

### Tier 0: platform core (Windows does not boot without it)

| # | Peripheral | SoC block / addr | Windows needs | Availability | Notes / firmware work |
|---|---|---|---|---|---|
| 0.1 | CPU: 4x Cortex-A55 + 3x A78 + 1x A78 (Kryo 670) | MPIDR 0x000-0x700, single DynamIQ cluster (K:204-400); capacity-dmips 1024/1946/1985 (K:214,315,387) | MADT GICC per core, including EfficiencyClass for big.LITTLE scheduling; PPTT; inbox `ACPI\Processor` (cpu.inf, fxppm) | High | Belongs to the ACPI workstream. Efficiency class 0 for cpu0-3, 1 for cpu4-6, 2 for cpu7 (**INFERRED** scheme). |
| 0.2 | GICv3 (GIC-600) | GICD 0x17a00000, GICR 0x17a60000 (8 x 128 KiB), ITS 0x17a40000 **status disabled** in K:6877-6895; 928 SPIs (dmesg) | MADT GICD/GICR (+ITS) | High (no ITS: PCIe falls back to INTx) | edk2-rockchip notes that SMMU & ITS "can't be enabled in Windows anyway" (`RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c:265-267`). PCIe INTx: K pcie0 interrupt-map, GIC_SPI 149. |
| 0.3 | Arch timer + memory-mapped timer | PPIs; timer-mem 0x17c20000 | GTDT | High | UEFI uses the virtual counter (E:`QCS6490.dsc.inc:101`). |
| 0.4 | PSCI 1.1 (OSI supported) | TZ (dmesg "PSCIv1.1", "OSI mode supported") | FADT PSCI_COMPLIANT; `_LPI` for idle | High | Core states 0x40000003/4 (K:443-474) only; avoid domain states without PEP (§3.2). |
| 0.5 | Watchdog | APSS WDT 0x17c10000 (K:6897) | Must be off at hand-off. Optional: `HalExtQCWdogTimer` (`ACPI\QCOM0604`, Q8B `HalExtQCWdogTimer8280`) | High | Confirm it is not armed at ExitBootServices. |
| 0.6 | LPDDR4X 8 GiB | SK hynix H9QT0G6CN6X146 uMCP: 128 GB UFS 2.2 + 8 GB LPDDR4X ([part][hynix]; dmesg `SKhynix H9QT0G6CN6X146`) | UEFI memory map | High (done: SMEM item 402) | DDR frequency is whatever the UEFI/AOP votes; Qualcomm Windows scales it through PEP/ICB. A persistent high BCM vote before hand-off is **INFERRED** to be needed for performance. |
| 0.7 | Debug UART5 (GENI SE5) | 0x994000 (K:1457); Micro-USB bridge on the board | DBG2/SPCR, port subtype 0x0011 or 0x0013 "SDM845" GENI ([DBG2 spec][dbg2]) | High | Lets kdcom and EMS work from the start. Subtype 0x0013 = 7.372 MHz clock. Which one matches the UEFI's SE5 clock is **INFERRED**; check it. |
| 0.8 | SMMU (apps SMMU-500) | 0x15000000 (K:6711) | Nothing if Windows sees no IORT and DMA is in bypass | High at EL2 | At EL2 after the Gunyah exit the SMMU is in bypass (README "Exception level"). At EL1 every inbox-driven stream needs a bypass entry kept past ExitBootServices; SmmuDxe now restores them (E:`Drivers/SmmuDxe/SmmuDxe.c:22-27`). |
| 0.9 | PMIC RTC (PMK8350) | SPMI 0xc440000, RTC at PMIC 0x6100 (dmesg `rtc-pm8xxx ... rtc@6100`; M:919-923 `allow-set-time`) | UEFI runtime GetTime/SetTime. Windows on Arm uses EFI runtime time when no `ACPI000E` device exists (**INFERRED** from RPi/RK practice) | Med (firmware work) | Today `VirtualRealTimeClockLib` (E:`QCS6490.dsc.inc:136`) gives a fake time. Write an SPMI PMIC RTC runtime library; the Q6A Qualcomm UEFI has a `RealTimeClock` driver. |
| 0.10 | TPM | none (fTPM in QSEE on Qualcomm PCs through QcTrEE: inbox `QcTrEE_i` `ACPI\QCOMFFEC`; Q8B `QcTrEE` `ACPI\QCOM04DE`) | TPM2 table + fTPM TA | Low | Bypass the setup check (LabConfig BypassTPMCheck/BypassSecureBootCheck). Windows 11 runs without a TPM. |

### Tier 1: needed for a usable install (storage, display, keyboard/mouse, network)

| # | Peripheral | SoC block / bus | Chip | Windows driver path | Likelihood | Firmware prerequisites |
|---|---|---|---|---|---|---|
| 1.1 | **UFS** (system disk) | UFS HC 0x1d84000 + QMP PHY 0x1d87000 (K:2471,2559); reset gpio175 (V:801-819, M:1130-1147) | SK hynix H9QT0G6CN6X146, UFS 2.2, LUN 0-7 (dmesg) | Inbox `storufs`, `ACPI\QCOM24A5` "UfsQualcomm8996Install" (FeatureFlags 0x40000000, HSSeries 2; WinPE storufs.inf:84,352-360). Also binds `ACPI\CC_010901`. | **High** (**INFERRED**: no PEP, so the controller must stay powered) | QcomUfsHcDxe leaves the link up. No PEP means GDSC, clocks and the RPMh rail votes (L7B, L9B; V:801-819) must persist. storufs presents each LUN as its own disk (**INFERRED**). DMA above 4 GiB is untested (§3.6). |
| 1.2 | **HDMI** | MDSS 0xae00000, DSI0 0xae94000, 7nm DSI PHY (K:5351,5471,5548) | Lontium LT9611 rev 0xE2 (dmesg) on I2C9 0x39 (V:2119-2169; M:735-775); reset gpio21, irq gpio20, 3.3 V enable gpio83 | (a) Inbox BasicDisplay (MSBDD) on the UEFI GOP framebuffer: fixed 1920x1080@60, no acceleration, no hot-plug. (b) `qcdx` (Radxa Q6A pack or WU `qcdx7280`) with ACPI panel XML for DSI0 plus an LT9611 helper (§4.1). | (a) **High**, after firmware changes; (b) Low-Med | (a) Keep the pipeline running at ExitBootServices (E:`MdssDisplayDxe.c:9-11,151-158` stop it today); the framebuffer is already reserved (:191-193); the MDSS stream must stay in bypass. |
| 1.3 | **USB 2.0 Type-A** | usb_2 DWC3 0x8c00000, HS PHY 0x88e4000 (K:4358,4287); host-only (M:1112-1116) | - | Inbox `usbxhci` via `ACPI\PNP0D10` (DWC3 in host mode is an xHCI), as edk2-rockchip does (`RK3588/AcpiTables/Usb3Host0.asl:12`). Optional `QcXhciFilter` (`ACPI\QCOM06A1` "Host mode Multiport", Q8B QcXhciFilter8280.inf:42). | **High** | Dwc3HostDxe leaves the core in host mode with the PHY powered (E:`Drivers/Dwc3HostDxe/Dwc3HostDxe.c:38,1051-1100`). Only one port: put keyboard, mouse and USB stick on a hub. |
| 1.4 | **USB 3.0 Type-A x2** | PCIe0 0x1c00000, Gen2 x1; DBI 0x60000000, config 0x60100000 (K:2209-2236) | Renesas uPD720201 (1912:0014), no EEPROM, FW 2026 (`usb-research/board-live.md:467-512`) | Inbox `usbxhci` (`PCI\CC_0C0330`) under inbox `pci.sys` | **Med**: depends on PCIe-for-Windows (§3.1) | RenesasXhciFwDxe downloads `renesas_usb_fw.mem` every boot; the chip keeps it while powered. No Windows ARM64 Renesas firmware loader exists. The supplies (gpio86/7/136, M:131-215, E:`Library/Qcs6490PciHostBridgeLib/Pcie0Init.c:221`) must stay on. |
| 1.5 | **Gigabit Ethernet** | USB port 3 of the Renesas (lsusb: Bus 3 Port 3 at 5 Gbit/s) | ASIX **AX88179B** (0b95:1790; Linux binds `cdc_ncm`; `usb-research/board/lsusb_irq.txt`) | Inbox `netax88179_178a` (v1.16.27.321, OS + WinPE, :21), or inbox `usbncm` (CDC-NCM class / MS_COMP_WINNCM, OS usbncm.inf:27). ASIX also offers a Win11 ARM64 HLK driver 4.22.1.0 ([ASIX][asixb]). | **Med** (High once 1.4 works) | gpio7 "vreg_eth_1v8" (M:131) must be on. Fallback: an RTL8153 dongle on usb_2 (inbox `rtucx21arm64`, OS + WinPE). |
| 1.6 | Keyboard/mouse | via 1.3 / 1.4 | USB HID | Inbox HID | High | - |

### Tier 2: useful, and a path exists

| # | Peripheral | SoC block / bus | Chip | Windows driver path | Likelihood | Notes |
|---|---|---|---|---|---|---|
| 2.1 | M.2 Key M, PCIe 3.0 x2, NVMe 2280, 3.3 V 2 A ([datasheet][tcds]) | PCIe1 0x1c08000, DBI 0x40000000; PERST gpio2, WAKE gpio3, power gpio56 (V:1699-1754; M:162,833-850) | (empty slot: "Device not found" in dmesg) | Inbox `pci.sys` + `stornvme` | Med | Needs PCIe1 bring-up in UEFI (README: "Not supported yet: ... PCIe1") plus the same ECAM handling as PCIe0. Windows could then also boot from NVMe. A Wi-Fi card through an M-to-E adapter (Qualcomm WCN685x, `PCI\VEN_17CB&DEV_1103`, `qcwlanhsp` in Q8B) is **INFERRED**. |
| 2.2 | USB-C: USB 3.1 Gen1 + DP 1.4 alt mode, PD 12 V 3 A input | usb_1 DWC3 0xa600000, QMP USB/DP combo PHY 0x88e8000 (K:4911,4306); orientation gpio140; SBU mux PI3USB102 gpio52/53 (V:1203-1236; M:349-367); pmic-glink connector (M:75-117) | HUSB238 PD sink controller on I2C15 0x08 (V:1604-1611); EUD on the HS path | Host only: `PNP0D10` with a fixed orientation (**INFERRED**). Dual-role: inbox `urssynopsys` (`QCOM24B6`) + UCSI from the ADSP pmic-glink (`qcusbcucsi7280` in WU; `QcPmicGlink8280` `ACPI\QCOM068E`, `qcusbcucsi8280` `ACPI\QCOM06A4` in Q8B). DP alt mode: `qcdx` DP. | Host Med; DP Low-Med | The port is also the board's power input, so host use needs a PD pass-through dock. HUSB238 is autonomous; no Windows driver needed. |
| 2.3 | Power button / "volume" keys | PMK8350 PON KPDPWR + RESIN (V:637-680; M:963-971); PM7325 GPIO6 (M:50-62) | - | `QcPmicApps` (`ACPI\QCOM062C`) + `qcpmicgpio` (`ACPI\QCOM062D`), Q8B. Or ACPI GED on the SPMI arbiter IRQ (**INFERRED**, complex). | Low-Med | Shutdown from the Start menu works through PSCI SYSTEM_OFF without these. |
| 2.4 | 40-pin header | I2C1 SE1 0x984000 (pins 3/5); SPI12 0xa90000 (19/21/23/24); UART2 0x988000 (8/10) (M:731,1010,1050); up to 28 GPIO, 2 I2C, 3 UART, 3 SPI, 1 I2S, 1 PWM ([datasheet][tcds]); header power enable gpio14 (V:1338-1344,1512-1520) | TLMM 0xf100000 (K:5827); QUP wrappers 0x9c0000 / 0xac0000 + GPI DMA | `qcgpio` (`QCOM060C`), `qci2c` (`QCOM0610`), `qcspi` (`QCOM060E`), `qcuart` (`QCOM0616`), `qcgpi` (`QCOM0688`) (Q8B INFs: qcgpio8280.inf:43, qci2c8280.inf:44, qcspi8280.inf:33, qcuart8280.inf:35). Inbox `qcgpio_i`/`qci2c_i` (WinPE). User-mode access through inbox `rhproxy` (`MSFT8000`). | Med | QUP SE firmware is already loaded by QupFwDxe (README). Qualcomm bus drivers use PEP for SE clocks (**INFERRED**). Without PEP the UEFI must leave the SE clocks on. WU 7280 has no `qcspi7280`; Q8B and Radxa have SPI. A Sense HAT on I2C1 is in the vendor DT (V:2180-2213), an accessory only. |
| 2.5 | CPU DVFS | EPSS cpufreq-hw 0x18591000 (K:7046-7047) | - | Qualcomm: PEP (`qcpep`, Q8B qcpep.wd8280.inf:33 `ACPI\QCOM0617`). Without PEP: ACPI CPPC `_CPC` pointing DesiredPerformance at the EPSS perf-state register, levels = LUT indices (**INFERRED**). | Med | Without either, CPUs stay at the firmware's frequency. |
| 2.6 | Thermal sensors | TSENS0 0xc263000, TSENS1 0xc265000 (K:5764-5775); PMK8350/PM7325 ADC thermistors xo/quiet/skin (V:612-671; M:867-961) | - | PEP TSENS (`ACPI\QCOM0637`... in qcpep.wd8280.inf), `qcadc` (`QCOM0611`). Or ACPI thermal zones with `_TMP` reading TSENS through a SystemMemory OperationRegion (acpi.sys; **INFERRED**), MPTF / MSTemperatureSensor inbox. | Med | Hardware LMh throttling in EPSS is autonomous (**INFERRED**). |
| 2.7 | Fan (4-pin PWM) | PM8350C LPG/PWM channel 3 on PM8350C gpio8 func1 (V:1114-1120,1996-2005; M:119-128) | - | None in Windows. Set a fixed duty in UEFI (LPG over SPMI). An ACPI fan `PNP0C0B` + `_FST`/`_FSL` writing LPG over SPMI is **INFERRED** and complex. | High (fixed duty) | Recommended: about 50-100 % at hand-off, since Windows has no fan curve. |
| 2.8 | RGB LED | PM8350C LPG channels 1-3 (V:588-610; M:892-917) | - | None needed; set a colour in UEFI | n/a | Cosmetic. |
| 2.9 | Bluetooth | uart7 GENI SE7 0x99c000, RTS/CTS, 3 Mbaud (V:1949-1994; M:1059-1087); shutdown gpio17, wake gpio39/137 | AP6256: BCM4345C5 (dmesg "BCM4345C5 (003.006.006)", patch `brcm/BCM4345C5.hcd`) | `qcuart` (SerCx2) + a Broadcom/Cypress UART BT serial-bus driver. WoR `cywbtserialbus.sys` works for CYW43455 on RPi 4 ([WoR README][wordrv]). Adapting it to BCM4345C5 with ACPI `UARTSerialBusV2` + GPIOs is **INFERRED**. | Low-Med | Licensing of the WoR/Cypress binary is open. Fallback: a USB Bluetooth dongle (inbox `bth.inf`). |
| 2.10 | ADSP / CDSP | ADSP remoteproc 0x3700000, CDSP 0xa300000 (K:4436,4761) | Hexagon 770 | `qcsubsys` (`QCOM061B`/`QCOM06B0`), `qcpil` (`QCOM06E0`), `qcglink` (`QCOM0684`), `qcipcc` (`QCOM06C2`), `qcscm` (`QCOM04DD`), `qcadsprpc` (`QCOM065C`) (Q8B qcsubsys8280.inf:45 etc.) | Med with the Radxa QCS6490 pack | Windows ADSP/CDSP images (audio and pmic-glink PDs) differ from the Linux AudioReach `adsp.mbn` (M:985-996; **INFERRED**). SC7280 WU firmware may not authenticate on QCS6490 (HW_ID; **INFERRED**). Under Windows, preloading DSPs in UEFI is not needed if `qcsubsys` loads them. |
| 2.11 | GPU Adreno 643 (Linux chip id 635.0) | 0x3d00000 + GMU 0x3d6a000 + adreno SMMU 0x3da0000 (K:3198-3391); zap shader `a660_zap.mbn` (V:584-586) | - | `qcdx` (Radxa Q6A: D3D12 FL12_1, Vulkan 1.3, OpenCL 3.0, GL 4.1 ([CNX][cnxq6a]); WU `qcdx7280`) | Low-Med | `qcdx` owns display **and** GPU. It needs the full Qualcomm ACPI (GPU0 device, panel XML through `_ROM`, PEP/ICB resources). With HDMI on LT9611 the display half is the blocker (§4.1). |
| 2.12 | Video codec (Venus / iris2) | 0xaa00000 (K:4988; M:1149) | - | Shipped inside the `qcdx` package: DXVA decode + `qcvidenc*` MFTs (Q8B DriverBinaryVersionInfo.csv lists `qcvidencarm64xmfth2648280.dll` etc.) | Low-Med | Tied to 2.11. Software decode works regardless. |

### Tier 3: unlikely, or substitute a USB device

| # | Peripheral | SoC block / bus | Chip | Windows path | Likelihood | Substitute |
|---|---|---|---|---|---|---|
| 3.1 | **Wi-Fi 802.11ac** | SDHC2 0x8804000 (K:4224), SDR104 SDIO (dmesg "SDR104 SDIO card"); WL_REG_ON gpio16, host wake gpio38, power gpio125 (V:1825-1883; M:225,998-1008 "WIFI part of the AP6256") | AP6256 = BCM43456 (chip 0x4345 rev 9, SDIO device 0xa9bf; vendor dhd log; brcmfmac "BCM4345/9", FW 7.45.96.61) | SD host: inbox `sdbus` `ACPI\QCOM24BF` (sdbus.inf:175) works, but no ARM64 Broadcom SDIO WLAN (NDIS/WDI) driver exists ([WoR][wordrv], [Infineon forum][ifx]) | **None** | A USB Wi-Fi adapter with an ARM64 driver, or a PCIe card in the M.2 slot (2.1). The on-board SoC Wi-Fi (WCN6750/WPSS) is not populated: mainline deletes `&wifi`/`&remoteproc_wpss` (M:23-35). |
| 3.2 | 3.5 mm headset (CTIA) | LPASS LPAIF primary MI2S + LPASS MCLK1 (V:79-218 sound card; V:1523-1602 codec); jack detect gpio63; codec enable gpio117 (V:1331-1336,1502-1510) | Everest **ES8316** on I2C0 0x11 (SE0 0x980000) | (a) Qualcomm audio stack (`qcadcm`/`qcaucd`/`qcaudminiport` + Windows ADSP) has no ES8316/MI2S topology. (b) Custom: an LPAIF MI2S + DMA driver modelled on Linux `lpass-sc7280` (K:2901 `lpass_cpu`) plus an ES8316 codec driver, like CoolStar's `csaudiork3x` + `rk3xi2sbus` + `es8323` (local `RkDrvPkg_ARM64_Release_v0.2_testsigned`). Needs LPASS clocks without ADSP ownership (**INFERRED**). | Low | USB audio (inbox `usbaudio2`). |
| 3.3 | HDMI audio | LPASS quaternary MI2S to LT9611 (V:159-176) | LT9611 | Same problem as 3.2 | Low | USB audio. |
| 3.4 | Cameras (2x 22-pin, 4-lane CSI D-PHY) | CAMSS / Spectra 570L 0xacb3000, CCI0/1 0xac4a000/0xac4b000 (K:5057-5137) | Supported modules: IMX219, IMX477, IMX708 ([Thundercomm][tccam]) | Qualcomm camera stack (`qccamisp7280`, `qccammipicsi7280`, `qccamplatform7280` + per-sensor drivers in WU). Radxa tested only IMX577 under Windows ([CNX][cnxq6a]). No Windows tuning or drivers for the RPi sensors. | Low | UVC webcam (inbox `usbvideo`). An IMX577 module could follow the Radxa path (**INFERRED**). |
| 3.5 | Modem / WPSS | MPSS/WPSS remoteprocs (vendor enables them, V:1794-1802; mainline deletes them, M:23-35) | none on QCS6490 | - | n/a | - |
| 3.6 | Camera PMICs PM8008 | I2C1 0x8/0x9/0xc/0xd, all `status = "disabled"` (V:909-1112) | - | - | n/a | Not populated or unused. |

---

## 3. Notes on cross-cutting problems

### 3.1 PCIe (PCIe0: Renesas, Ethernet, 2x USB 3; PCIe1: M.2)

- **Windows wants ECAM.** The SC7280 DWC root complex exposes the root port in DBI (0x60000000) and bus 1 through an iATU CFG window (0x60100000). The PCIe0 window is only 0x60000000-0x64000000 (K:2211-2228).
- **Known recipe from edk2-rockchip** for DWC under Windows (`RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c:243-295`):
  - FADT OEM ID "NXPMX6", which triggers Windows' i.MX6 device filter on buses 0/1.
  - Split config spaces.
  - A root-port resource device (`Pcie.asl:69-79`).
  - Linux 7.0.2 also has an iATU "CFG shift" ECAM mode (`drivers/pci/controller/dwc/pcie-designware-host.c:420-459`). It needs a 256 MiB-aligned config base (:488-501). 0x60000000 is aligned, but the 64 MiB window limits the bus range.
- **MSI:** the ITS is disabled in DT (K:6888-6893). Plan on INTx `_PRT` (GIC SPI 149+ for PCIe0, K pcie0 interrupt-map). The uPD720201 supports INTx (**INFERRED**: `usbxhci` falls back to line interrupts).
- This belongs to the PCIe/ACPI workstream. It gates items 1.4, 1.5 and 2.1.

### 3.2 Power management without Qualcomm PEP

- On Qualcomm Windows PCs, `qcpep` (Q8B `ACPI\QCOM0617`) owns:
  - device power (F-states and D-states, clocks, GDSCs);
  - RPMh votes and interconnect bandwidth (ICB);
  - TSENS;
  - CPU performance.
- PEP is configured by large ACPI resource packages.
- **Inbox drivers do not need PEP.** They work if the UEFI leaves the hardware on.
- **The UEFI's RPMh votes are ACTIVE_ONLY** (E:`Library/Qcs6490RpmhLib/Rpmh.c:1-12`, TCS 0/1 AMC). An apps-subsystem power collapse (RSC sleep) applies the sleep set, which this DRV never wrote.
  - The OS must therefore never request the domain idle states (K:481-501).
  - Alternatively, the UEFI writes matching sleep/wake TCS votes before hand-off. **INFERRED**.
- **DDR and NoC bandwidth** stay at the last vote (see `hdmi-research/power-bandwidth`). **INFERRED** performance impact.
- **Qualcomm drivers (qcdx, qcsubsys, audio) require PEP.** Treat "PEP + Qualcomm DSDT" as one later milestone. Sources:
  - The Radxa Q6A Windows UEFI's DSDT. Radxa's download page now lists `flat_build_260120` ([downloads][radxadl]). That it has ACPI is **INFERRED**; the local copy is 251013.
  - Project Aloha `KodiakPkg/Device/qcom-qrd7325/ACPI/DSDT.aml` (BSD-2 repo, binary AML, not downloaded) ([repo][aloha]).

### 3.3 Display: BasicDisplay first

Requirements:
- No `DisplayStop` at BeforeExitBootServices when booting Windows. Detect the OS the way edk2-rockchip does: `Silicon/Rockchip/Drivers/ExitBootServicesHookDxe/OsIdentification.c`.
- The LT9611 stays powered and keeps TMDS on.
- The framebuffer stays in `EfiReservedMemoryType` (already done).
- The MDSS stream stays valid: EL2 bypass, or keep the Gunyah bypass S2CR.
- The DSI PHY PLL, dispcc and MDSS GDSC stay on. Nothing in Windows touches them without `qcdx`.

Limits: one fixed mode (1080p60), and no hot-plug re-train. A TV that is power-cycled may need the LT9611 HPD path; that is **INFERRED** to be harmless, because TMDS stays on.

### 3.4 Storage-adjacent

- **Install media:** USB stick on usb_2 (High) or on the Renesas ports (Med).
  - SD card: none. SDHC2 is the Wi-Fi SDIO; there is no µSD slot on this board ([datasheet][tcds] lists none).
  - The UFS variable store (`logfs` on LUN 4) and the Linux partitions on LUN 0-5 must not be wiped by Windows Setup. Install to a free area or a dedicated LUN. **INFERRED**: Windows sees each LUN as a separate disk through storufs.
- **Runtime variables:** Windows Setup (bcdboot) writes Boot####/BootOrder at runtime. The current firmware does not persist runtime writes (README "Settings").
  - The calls must at least succeed in memory.
  - Booting then relies on `\EFI\BOOT\BOOTAA64.EFI` and the per-LUN boot options the UEFI creates.

### 3.5 Signing

| Driver source | Signing | Consequence |
|---|---|---|
| WU 7280 drivers | Production-signed (Windows Update) | Load with test signing off. Their INFs target SC7280 reference HIDs/SUBSYS (CLS/CRD) and may check the SoC ID (**INFERRED**). |
| Radxa Q6A pack | Test-signed | Needs `bcdedit /set testsigning on`, which in turn needs Secure Boot off (it is off). |
| Q8B pack | Radxa test cert | - |

### 3.6 DMA above 4 GiB

The README notes usb_2 DMA above 4 GiB timed out under Gunyah. Windows `usbxhci` and `storufs` use 64-bit DMA when AC64 / 64AS is set. Verify at EL2. Otherwise a `_DMA` limit or a 4 GiB memory cap (`truncatememory`, debug only) is needed. **INFERRED** risk.

---

## 4. HDMI: can Qualcomm's Windows display driver drive DSI0 + LT9611?

### 4.1 Evidence

- **Qualcomm UEFI panel XML.** The Q6A flat build's Qualcomm UEFI (FV3) contains `Panel_lt9611uxc_dsi2hdmi_vid.xml`: file `windows-research/q6a-fw/out/uefi_elf/FV1/D14C51ED-4F4D-2F2D-EAE4-77914692D046_fv/FV3/4C266D6B-77A7-44B8-BDA9-7D0601BB662B_1.raw`.
  - It describes the RB3 Gen 2's **LT9611UXC** as an ordinary DSI video-mode "panel": 1920x1080, HFP 88 / HBP 148 / HSW 44, VFP 4 / VBP 36 / VSW 5, `DSILanes` 4, `DisplayResetGpio` 111, `InterfaceType` 8.
  - The UXC has its own MCU that handles HDMI, EDID and HPD, so the SoC only feeds DSI.
- **Configuration source on Windows.** Qualcomm Windows display configuration comes from ACPI. The same panel-XML schema is returned through the GPU device's `_ROM`; the UEFI and ACPI copies are separate (Qualcomm 80-NB116-2 guide, summarized in [96boards][96b]).
  - The DragonBoard 410c drove an ADV7533 DSI-HDMI bridge under Windows 10 IoT Core through this mechanism. **INFERRED**: the exact I2C-programming tags were not verified.
- **The RUBIK Pi 3 has the plain LT9611** (revision 0xE2), which must be programmed over I2C. That covers MIPI RX port B, TX PLL, PCR locked to the DSI clock, InfoFrames and TMDS; see E:`Drivers/MdssDisplayDxe/Lt9611.c:1-22,40-45,276-290`.
  - Thundercomm implements this for its Qualcomm UEFI as "LT9611 MDPPlatformLib", announced to be open-sourced in March 2026 ([forum][rubikforum]).
  - No public Windows-side LT9611 component was found.

### 4.2 Options, best first

1. **BasicDisplay on the UEFI GOP** (§3.3). High likelihood, no acceleration. Everything else (USB, UFS, Ethernet) is independent of `qcdx`.
2. **`qcdx` with a fixed 1080p60 DSI0 panel XML.**
   - The UEFI leaves the LT9611 configured. After `qcdx` re-initializes DSI, the LT9611 PCR may need a reset (0x8011: 0x5A then 0xFA, Lt9611.c:40-45) and a video check.
   - That needs either a tiny KMDF helper on I2C9 that waits for the display power-on, or ACPI panel power methods, if `qcdx` calls them. **INFERRED**; must be tested.
   - No EDID-driven mode switching.
3. **USB-C DisplayPort alt mode via `qcdx`.** Native DP path, no bridge. Radxa's Q6A HDMI is DP0 through a transparent RA620 DP-to-HDMI converter and works with `qcdx` ([search result citing Radxa schematic][radxasch]).
   - Needs pmic-glink/UCSI or a forced orientation, the QMP DP PHY, and PEP.
   - Medium, and only with a DP/USB-C monitor.
4. **A full Windows LT9611 bridge driver** co-operating with `qcdx`. Not supported by any public Qualcomm interface. **INFERRED** Low.

---

## 5. Firmware (UEFI) work this inventory implies, in order

1. Keep HDMI alive past ExitBootServices for Windows: E:`MdssDisplayDxe.c:151-158`, SmmuDxe restore (`SmmuDxe.c:22-27`). Prefer EL2 for Windows.
2. ACPI tables, from the ACPI workstream:
   - FADT, MADT (EfficiencyClass), GTDT, PPTT, DBG2/SPCR (GENI subtype).
   - DSDT with UFS0 `QCOM24A5`, USB2 `PNP0D10`, the PCIe0 host bridge (NXPMX6 / AMZN0001-style DWC handling), and INTx `_PRT`.
3. PMK8350 RTC runtime library to replace `VirtualRealTimeClockLib` (E:`QCS6490.dsc.inc:136`).
4. Runtime SetVariable must succeed, at least volatile.
5. Fan fixed duty plus LED at hand-off (PM8350C LPG over SPMI).
6. PCIe1 bring-up for the M.2 slot. Optional: usb_1 host mode with orientation from gpio140.
7. Later: Qualcomm DSDT plus PEP to enable `qcdx`, `qcsubsys`, Qualcomm I2C/SPI/UART/GPIO, CPU DVFS and thermal. Get Radxa's QCS6490 Windows UEFI DSDT and driver pack (user decision; not downloaded here).

---

## 6. Sources

Local:
- Vendor DTS `qcs6490-thundercomm-rubikpi3.dtsi` (V), mainline DTS (M), `kodiak.dtsi` (K).
- Linux 7.0.2 tree `<flange>/.build/sources/repos/893346ef.../`.
- Board logs: `workspace/usb-research/board/{dmesg_full.txt,lsusb_irq.txt}` and `workspace/hdmi-research/live-capture/journal_boot-2_stockuefi_kernel.txt`. These record the dhd BCM4345 rev 9, ES8316, LT9611 rev 0xE2, SK hynix UFS, AX88179B and Renesas.
- Win11 25H2 ARM64 ISO `boot.wim` and `install.wim` inbox INFs (extracted to the scratchpad only).
- Q8B driver pack INFs and `DriverBinaryVersionInfo.csv`.
- Rockchip pack `RkDrvPkg_ARM64_Release_v0.2_testsigned` (CoolStar audio/I2S/GPIO/I2C, ES8323).
- edk2-rockchip `RK3588/AcpiTables`, `AcpiPlatformDxe.c`.
- Q6A flat build `uefi.elf` module inventory (`windows-research/q6a-fw/out/inventory.tsv`).

Web:
- [mslist]: https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/winiot_qualcomm_processors_2024
- [woarepo]: https://github.com/WOA-Project/Qualcomm-Reference-Drivers (folder `7280_WINDOWS_CLS`)
- [woachg]: https://raw.githubusercontent.com/WOA-Project/Qualcomm-Reference-Drivers/master/7280_WINDOWS_CLS/CHANGELOG.md
- [radxawin]: https://docs.radxa.com/en/dragon/q6a/other-system/windows
- [radxadl]: https://docs.radxa.com/en/dragon/q6a/download
- [cnxq6a]: https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/
- [radxasch]: https://dl.radxa.com/q6a/hw/radxa_dragon_q6a_schematic_v1.21.pdf (RA620 DP-to-HDMI; from search results, not opened)
- [rubikforum]: https://community.rubikpi.ai/t/windows-on-arm-on-the-rubik-pi-3/521
- [tcds]: https://www.thundercomm.com/rubik-pi-3/en/docs/rubik-pi-3-user-manual/1.0.0-u/datasheet/
- [tccam]: https://www.thundercomm.com/rubik-pi-3/en/docs/peripheral-compatibility-list/
- [hynix]: https://www.preduo.com/product/umcp/ufs-lpddr4x/254ball_ufs-lpd4x/h9qt0g6cn6x146n
- [asixb]: https://www.asix.com.tw/en/product/USBEthernet/Super-Speed_USB_Ethernet/AX88179B
- [wordrv]: https://github.com/worproject/RPi-Windows-Drivers/blob/master/README.md
- [ifx]: https://community.infineon.com/t5/AIROC-Wi-Fi-and-Wi-Fi-Bluetooth/Driver-Support-for-CYW43455-in-Windows-10-on-ARM64/td-p/274475
- [dbg2]: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-debug-port-table
- [aloha]: https://github.com/Project-Aloha/mu_aloha_platforms (`Platforms/KodiakPkg/Device/qcom-qrd7325/ACPI/DSDT.aml`)
- [96b]: https://discuss.96boards.org/t/display-drivers-acpi-and-xml-configuration-guide-what-does-displayplatformid-and-panelid-means/4782

[mslist]: https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/winiot_qualcomm_processors_2024
[woarepo]: https://github.com/WOA-Project/Qualcomm-Reference-Drivers
[woachg]: https://raw.githubusercontent.com/WOA-Project/Qualcomm-Reference-Drivers/master/7280_WINDOWS_CLS/CHANGELOG.md
[radxawin]: https://docs.radxa.com/en/dragon/q6a/other-system/windows
[radxadl]: https://docs.radxa.com/en/dragon/q6a/download
[cnxq6a]: https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/
[radxasch]: https://dl.radxa.com/q6a/hw/radxa_dragon_q6a_schematic_v1.21.pdf
[rubikforum]: https://community.rubikpi.ai/t/windows-on-arm-on-the-rubik-pi-3/521
[tcds]: https://www.thundercomm.com/rubik-pi-3/en/docs/rubik-pi-3-user-manual/1.0.0-u/datasheet/
[tccam]: https://www.thundercomm.com/rubik-pi-3/en/docs/peripheral-compatibility-list/
[hynix]: https://www.preduo.com/product/umcp/ufs-lpddr4x/254ball_ufs-lpd4x/h9qt0g6cn6x146n
[asixb]: https://www.asix.com.tw/en/product/USBEthernet/Super-Speed_USB_Ethernet/AX88179B
[wordrv]: https://github.com/worproject/RPi-Windows-Drivers/blob/master/README.md
[ifx]: https://community.infineon.com/t5/AIROC-Wi-Fi-and-Wi-Fi-Bluetooth/Driver-Support-for-CYW43455-in-Windows-10-on-ARM64/td-p/274475
[dbg2]: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-debug-port-table
[aloha]: https://github.com/Project-Aloha/mu_aloha_platforms
[96b]: https://discuss.96boards.org/t/display-drivers-acpi-and-xml-configuration-guide-what-does-displayplatformid-and-panelid-means/4782
