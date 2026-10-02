# Windows 11 on Arm (ARM64): platform and firmware requirements, mapped to the RUBIK Pi 3 EDK2

Scope: the requirements Windows 11 ARM64 (25H2 media; also IoT Enterprise LTSC 2024) places on
firmware and platform, as a checklist against `edk2-qualcomm` (QCS6490 / SC7280 "kodiak").
Status 2026-10-02. Paths are relative to `.` unless absolute.
`[n]` = source list at the end. **INFERRED** = my conclusion, not stated by a source or verified on hardware.

Legend for "Need": **BOOT** = needed to get past bootmgr/winload/kernel; **INST** = needed for Setup
(install from USB to UFS, reboot into it); **RUN** = needed for a usable system; **CERT** = only for
logo/WHCP compliance or optional features.

Supporting evidence produced for this report: `workspace/windows-research/win-reqs/`
(`acpi-sig-refs.txt` = ACPI signatures loaded by bootmgr/winload/ntoskrnl, from the shared disassembly in
`workspace/windows-research/dis/`; `kdcom-dbg2-subtypes.txt` + `kdcom.text.asm` = DBG2 serial subtypes
the 25H2 kernel debugger supports).

---------------------------------------------------------------------------------------------------

## 0. Verdict in one table

| Area | Windows needs | Our firmware today | Gap |
|---|---|---|---|
| ACPI | RSDP/XSDT + FADT(HW-reduced, PSCI) + MADT + GTDT + DSDT (+MCFG for PCIe, DBG2 for debug) | **No ACPI at all**, DT only (`DtPlatformDxe`, QCS6490.dsc.inc:641) | **Blocker** (bootmgr dies at BlInitializeLibrary, no FACP) |
| GOP after ExitBootServices | Frame buffer must keep scanning out after EBS [3] | `MdssDisplayDxe` stops the display in BeforeExitBootServices (MdssDisplayDxe.c:9-11, 140-160) | **Blocker for any display in Windows** (BasicDisplay only inherits the GOP FB [9]) |
| DMA after EBS (Gunyah, EL1) | Inbox storufs/usbxhci DMA with physical addresses (no inbox MMU-500 driver; INFERRED) | `SmmuDxe` puts S2CRs back at EBS (SmmuDxe.c:22-27, 714-715) | **Blocker at EL1**; not an issue at EL2 (SMMU left in bypass, SmmuDxe.c:15-16) |
| Runtime SetVariable | "All of the UEFI Variable Services are required" [3]; Setup's bcdboot writes Boot#### at runtime | Runtime writes fail with EFI_WRITE_PROTECTED (NvStoreFvbDxe.c:33-36, 1658-1659, 1716-1717) | **Likely Setup blocker** (INFERRED) - at least make writes succeed in RAM |
| RNG | EFI_RNG_PROTOCOL "MANDATORY" on SoC [3] | none (`BaseRngLibTimerLib`, QCS6490.dsc.inc:151, no RngDxe) | Should fix (SC7280 TRNG at 0x010D3000, kodiak.dtsi:2109-2112) |
| Time | GetTime/SetTime (used before EBS only) [3] | VirtualRealTimeClockLib (QCS6490.dsc.inc:136) | OK to boot; better: PMK8350 RTC |
| ResetSystem | reset + shutdown [3] | ArmPsciResetSystemLib (QCS6490.dsc.inc:106) | OK |
| SMBIOS | >= 2.4; Type 0/1/2/3 fields for CHID [11] | 3.7 (QCS6490.dsc.inc:476), but UUID all-zero (OemMiscLib.c:361-366), SN/SKU placeholders (RubikPi3.dsc:96-97) | Fix UUID/serial (RUN/CERT) |
| TPM 2.0 | Win11 consumer Setup check [12][13]; optional for IoT Enterprise [15] | none (TpmMeasurementLibNull, QCS6490.dsc.inc:157) | Bypass (LabConfig) or use IoT Enterprise LTSC |
| Secure Boot | "capable" for Setup check; WHCP: ship enabled [3][12] | off; AuthVariableLibNull (QCS6490.dsc.inc:158) | Bypass for now; implement later (needs persistent auth vars) |
| CPU | ARMv8.1 atomics (PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE) [12 §3.5]; QCS6490 on 24H2 list [16] and IoT list [17] | Kryo 670 = A78/A55 (v8.2) | OK |
| RAM / storage | 4 GB / 64 GB [12 §3.7-3.8] | 8 GB / 128 GB UFS (LUN0) | OK (4Kn ESP >= 300 MB [22]) |
| Debug | DBG2 debug port "required" [1][2] (compliance), serial or KDNET | nothing | GENI UART **is** a supported DBG2 subtype (0x11/0x13), KDNET-EEM on usb_1 DWC3 possible |

The two firmware changes that gate *everything*: (1) publish ACPI tables, (2) make the hand-over
"Windows-safe" (display keeps running, DMA streams stay passable, runtime variable writes do not fail).

---------------------------------------------------------------------------------------------------

## 1. ACPI tables

General: Windows locates the RSDP through the EFI configuration table and prefers the XSDT [1].
SoCs are "hardware-reduced" ACPI platforms: no PM timer, RTC alarm, SCI, fixed registers, GPE, EC [6].
Evidence of what the 25H2 binaries actually load (literal-pool signature loads, `win-reqs/acpi-sig-refs.txt`):
bootmgr: FACP, XSDT, MCFG, BGRT, CSRT, SPCR, SRAT; winload: FACP, APIC, GTDT, DBG2, DBGP, SPCR, CSRT,
PPTT, MCFG, BGRT, TPM2, WDAT, WPBT, SRAT, PRMT, RSDT/XSDT; ntoskrnl: FACP, FACS, APIC, GTDT, IORT, PPTT,
CSRT, MCFG, DSDT, FPDT, WDAT, WPBT, SRAT, SLIT, PRMT. (bootmgr FADT lookup is fatal: `dis/bootmgr.asm`
0x101af6c0-0x101af6d0, the 0xc0000225 already established.)

### 1.1 Checklist

| Table | Need | Windows evidence | Content for QCS6490 (source) | Status / work |
|---|---|---|---|---|
| RSDP (rev 2) + XSDT | BOOT | [1]; bootmgr uses XSDT | XSDT only (no RSDT): edk2 `AcpiTableDxe`, `PcdAcpiExposedTableVersions = 0x20`-style (INFERRED choice) | Missing |
| FADT (FACP) | BOOT | bootmgr BlInitializeLibrary fatal without it; [1][6] | Rev 6 (6.0-6.3). Flags: HW_REDUCED_ACPI (bit 20) mandatory; LOW_POWER_S0_IDLE (bit 21) **off** until a PEP/_LPI exists (Qualcomm sets both, `WOA-Project 8250/src/FACP.dsl`; Rockchip sets only HW_REDUCED, edk2-rockchip Fadt.aslc:64). ArmBootArchFlags = PSCI_COMPLIANT, **no** PSCI_USE_HVC: conduit is SMC both under Gunyah and at EL2 (kodiak.dtsi:866-868 `method = "smc"`; Qualcomm FACP.dsl: PSCI 1, HVC 0). Hypervisor Vendor Identity: Qualcomm writes "QCOM" (FACP.dsl) - leave 0 unless we implement QHEE's Secure-Launch API (INFERRED, see 4). IaPcBootArch: Rockchip sets MSI_NOT_SUPPORTED to force INTx (Fadt.aslc:61). | Missing |
| MADT (APIC) | BOOT | ntoskrnl loads APIC (5 sites); [1] boot CPU first | 8x GICC (ACPI 6.0 layout is enough, edk2-rockchip Madt.aslc:41/55): UID 0-7, MPIDR 0x000..0x700 (kodiak.dtsi:204-388), GICC phys base 0 (sysreg GICv3), perf IRQ GSIV 23 (PPI 7, kodiak.dtsi:855-863), VGIC maint GSIV 25 (PPI 9, kodiak.dtsi:6881), GICR base 0 per-CPU + one GICR range 0x17A60000/0x100000; Efficiency Class 0 (CPU0-3 A55), 1 (CPU4-6 A78), 2 (CPU7 A78 prime) from capacity 1024/1946/1985 (kodiak.dtsi:214/315/387) - Qualcomm 8250 uses 0/1 (APIC.dsl). GICD 0x17A00000, version 3. **No ITS** (DT has it `status = "disabled"`, kodiak.dtsi:6888-6894; vendor sc7280.dtsi:7601-7607). Optional GIC MSI frame: Qualcomm 8250 uses base 0x17A10000 (GICD SETSPI frame), SPI base 0x340, count 0x80 (APIC.dsl); on SC7280 the same frame is live (Wi-Fi writes 0x17A10040, kodiak.dtsi:2166-2168) and DT uses SPIs only up to 799, so INTID 832-959 look free (INFERRED; verify GICD_TYPER). Note edk2-rockchip disabled its MSI frame "as some OSes are not happy about it (like Windows)" (Madt.aslc:181-187). | Missing |
| GTDT | BOOT | ntoskrnl `fn_14049f2e8` loads GTDT then requires CNTFRQ_EL0 != 0, else 0xC0000182 (`dis/ntos.asm` 0x14049f304-0x14049f32c); [1] | Rev 2 (ACPI 5.1; Rockchip Gtdt.aslc:51). CntControlBase/CntReadBase = ~0. PPIs: secure EL1 29, NS EL1 30, virtual 27, NS EL2 26 (kodiak.dtsi:7881-7887 PPI 13/14/11/10) - identical to Qualcomm 8250 GTDT.dsl. GT block 0x17C20000, frame 0 0x17C21000 / EL0 0x17C22000, GSIV 40 / 38 (SPI 8/6, kodiak.dtsi:6905-6918), always-on (as Qualcomm). **No SBSA watchdog** (none on SC7280; the APSS WDT is "Owned by Gunyah hyp", kodiak.dtsi:6897-6903). | Missing |
| DSDT | BOOT (namespace) / INST (devices) | ACPI.sys; [1] | See 1.2. | Missing |
| MCFG | INST (only if PCIe is used: the 2x USB 3.0 ports + AX88179 are behind PCIe0) | bootmgr/winload/ntos load MCFG; [27] BSA ECAM | Entry base 0x60000000, seg 0, bus 0-1: exactly Qualcomm's own WoA layout (8250 MCFG.dsl: 0x60000000 bus 0-1, 0x40000000 bus 0-1) and our DBI@0x60000000 + 1 MiB iATU CFG window @0x60100000 (kodiak.dtsi:2209-2217). DWC is not ECAM-clean (aliasing on bus 0/1); edk2-rockchip solves this for Windows with the "NXPMX6" FADT OEM-ID quirk that makes Windows filter duplicates on bus 0/1 (AcpiPlatformDxe.c:248, 260-294). Interrupts: INTx via `_PRT` (SPI 149-152 = GSIV 181-184, kodiak.dtsi:2249-2252) unless the MSI frame works. | Missing |
| DBG2 | CERT ("Microsoft requires a debug port on all systems" [1][2]); BOOT not needed (INFERRED) | winload loads DBG2 4x | Entry 0: Serial 0x8000 / subtype **0x0013** or **0x0011** (QUP GENI, see 6), base 0x00994000, GAS 32-bit access, namespace `\_SB.UAR5` (uart5, kodiak.dtsi:1457-1464). Qualcomm 8250 uses 0x8000/0x0011 @0xA90000 (DBG2.dsl). Optional entry: KDNET-EEM 0x8003/0x5143 on usb_1 DWC3 0x0A600000 (Qualcomm 8250: sizes 0xFFFFF/0x1000, 148-byte OEM data, namespace `\_SB.URS0`). | Missing |
| SPCR (rev >= 2) | CERT (EMS/headless) | bootmgr/winload load SPCR | Interface type = DBG2 subtype (rev >= 2 [4]), GSIV 638 (SPI 606), 115200 (value 7). Qualcomm's 8250 set has no SPCR. | Optional |
| PPTT | RUN (topology/scheduling) | winload/ntos load PPTT | **Revision 1 (ACPI 6.2)**: Windows 22621 doubles L3 with rev 2 and bugchecks 0x7E on rev 3 cache ID (edk2-rockchip Pptt.aslc:5-10, 187); Qualcomm 8250 also ships rev 1 (PPTT.dsl). | Optional |
| IORT | RUN only if an SMMU or ITS is exposed | ntos loads IORT | Phase 1: **omit** (no ITS; SMMU left passable). Qualcomm WoA describes the MMU-500 as an SMMUv1/v2 node + named components (8250 IORT.dsl: 0x15000000, 128 ctx IRQs) for its own `qcsmmu`/`qciommu` drivers (Q8B pack: ACPI\QCOM0609, ACPI\QCOM068F). | Later |
| CSRT | RUN only with Qualcomm HAL extensions | ntos/winload/bootmgr load CSRT; [1] "must be included ... if non-standard CSRs are used" | Not needed for inbox drivers. Qualcomm uses it for GPI DMA (8250 CSRT.dsl groups QCOM 0x100B/0x100C) and the watchdog HAL extension (Q8B `HalExtQCWdogTimer8280`, ACPI\QCOM0604). | Later |
| BGRT | CERT (logo continuity) | bootmgr/winload load BGRT | edk2 `BootGraphicsResourceTableDxe` + existing BootLogoLib (QCS6490.dsc.inc:176). | Optional |
| FPDT | CERT | ntos loads FPDT; [1] lists it | edk2 FirmwarePerformanceDxe. | Optional |
| TPM2 | INST for consumer Win11 without bypass; RUN for BitLocker/Hello | winload loads TPM2 | Only with a TPM. Qualcomm uses start method 9 (8250 TPM2.dsl; 9 is "reserved" in ACPICA actbl3.h:465) with its QcTrEE stack. | Not planned |
| WDAT, SDEV, WSMT, SRAT/SLIT, PRMT, WPBT | - | referenced, optional | WSMT is x86/SMM; none needed. | - |

### 1.2 Minimum DSDT for "install from USB, boot from UFS" with inbox drivers only

Inbox hardware IDs below are from the 25H2 WinPE image (boot.wim index 2, extracted INFs in
`workspace/windows-research/fwgap/2/Windows/INF/`):

| Device | `_HID`/`_CID` Windows binds | Resources (DT) | Notes |
|---|---|---|---|
| CPUs | `ACPI0007` (+ `ACPI0010` containers), `_UID` = MADT UID; cpu.inf:29 binds `ACPI\Processor`, machine.inf:82 `ACPI0010` | - | Pattern: edk2-rockchip Cpu.asl:16-37. `_LPI` later (PSCI params 0x40000003/0x40000004, cluster 0x41000044..., kodiak.dtsi:440-500; DT uses OS-initiated domains). |
| UFS host | `QCOM24A5` ("Qualcomm UFS Host Controller", storufs.inf:84, install section `UfsQualcomm8996Install`) or generic `_CLS` 01/09/01 (`ACPI\CC_010901`, storufs.inf:64) | 0x01D84000/0x3000, SPI 265 = GSIV 297, `_CCA 1` (dma-coherent, kodiak.dtsi:2471-2487) | Inbox driver expects PHY/clocks/link already up: firmware must leave UFS initialised (no EBS teardown today - good). Windows sees every LUN as a disk; LUN1/2/4/5 hold boot firmware (INFERRED risk: hide or warn). |
| USB 2.0 Type-A (usb_2 DWC3 host) | `PNP0D10` (usbxhci.inf:39) | 0x08C00000/0xFC100, SPI 242/241/240 (kodiak.dtsi:4378-4380), `_CCA 0` (not dma-coherent), SID 0xA0 (kodiak.dtsi:4398) | Pattern: edk2-rockchip Usb3Host0.asl:12-19 (whole DWC3 block). Host mode already set by `Dwc3HostDxe`, no EBS teardown (good). |
| PCIe0 root (uPD720201 + AX88179) | `PNP0A08`/`PNP0A03` (pci.inf:28-29) -> Renesas xHCI `PCI\CC_0C0330` (usbxhci.inf) | MCFG above; `_CRS` bus 0-1, MEM 0x60300000+0x3D00000, IO 0x60200000 (kodiak.dtsi:2226-2227); `_PRT`; `_OSC`; `_CCA 1` (kodiak.dtsi:2294) | Renesas FW must already be loaded (RenesasXhciFwDxe) and power must never be removed by Windows: expose no `_PR3`/D3cold (INFERRED). |
| Debug UART | any `_HID` (e.g. vendor ID), referenced by DBG2/SPCR namespace string | 0x00994000, GSIV 638 | Lets Windows arbitrate kd vs driver use [2]. |
| USB-C (usb_1 DWC3) for KDNET-EEM | `QCOM24B6` / `PNP0CA1` = "Synopsys USB 3.0 Dual-Role Controller" (urssynopsys.inf:34) | 0x0A600000 | Only with the DBG2 0x8003/0x5143 entry. |
| Time & Alarm (optional) | `ACPI000E` (acpitime.inf:27) | needs a PMIC RTC access path from AML - not practical without Qualcomm SPMI drivers (INFERRED) | Skip; UEFI GetTime suffices for boot. |
| Motherboard resources | `PNP0C02` (machine.inf:65) | DBI/ELBI/ATU 0x60000000-0x60002000 etc. | Keeps Windows from assigning them. |

Display needs no DSDT device: BasicDisplay binds `ROOT\BASICDISPLAY` (basicdisplay.inf:48) and uses
the GOP frame buffer [9].

### 1.3 Known Windows ACPI quirks to copy (from edk2-rockchip, which boots Win11 in ACPI mode [24])

- PPTT rev 1 (Pptt.aslc:5-10); MADT GICC ACPI 6.0 structures (Madt.aslc:55); no MSI frame/ITS when broken (Madt.aslc:181-202).
- PCIe on a non-ECAM DesignWare: "NXPMX6" OEM ID in FADT + MCFG split/filtering (AcpiPlatformDxe.c:248-294); INTx through `_PRT` (Pcie.asl:27) and `_OSC` (Pcie.asl:93-96).
- OS-specific fix-ups at ExitBootServices: Rockchip identifies the Windows loader (`IsPeImageWinLoader`, OsIdentification.c:46, 112-113) and patches the DSDT (AcpiPlatformDxe.c:408-421). The same hook is the natural place for our Windows-only EBS behaviour (display, SMMU) - see 2.
- DT vs ACPI selection: `ConfigTableMode` ACPI / DT / Both (RK3588Dxe/ConfigTable.c:80-87, AcpiPlatformDxe.c:573, FdtPlatformDxe.c:1418). For us "Both" is plausible (Linux picks DT, Windows ignores the FDT table; INFERRED), but a switch is cheap insurance.

---------------------------------------------------------------------------------------------------

## 2. UEFI requirements [3][5]

| Requirement (source) | Need | Our firmware | Action |
|---|---|---|---|
| UEFI >= 2.3.1, Class 3, no CSM [3][5][12 §3.6.2] | BOOT | edk2 | OK |
| Config table: ACPI RSDP GUID and SMBIOS GUID "must" be present [3] | BOOT | SMBIOS yes, ACPI no | add ACPI |
| Boot services: all memory services, OpenProtocol/CloseProtocol/LocateDevicePath/LocateHandle, ExitBootServices, deterministic Stall() [3] | BOOT | edk2 | OK |
| Runtime **time**: GetTime/SetTime, "only be called during boot (before ExitBootServices())" [3] | BOOT | VirtualRealTimeClockLib (QCS6490.dsc.inc:136) | OK; better real time from PMK8350 RTC (SPMI, 0x6100; writable on this board: rubikpi3.dts:919-923 `allow-set-time`, pmk8350.dtsi:71-77). Linux keeps a Qualcomm `RTCInfo` offset variable for read-only RTCs (rtc-pm8xxx.c:88-93) - not needed here. |
| Runtime **variables**: "All of the UEFI Variable Services are required" [3]; Data protection: NV + BS + authenticated-write attributes [3 Req 27] | INST | runtime writes -> EFI_WRITE_PROTECTED; RT_PROPERTIES says no SetVariable (NvStoreFvbDxe.c:1773-1794) | **P0**: let runtime writes succeed into the in-memory store (RPi does exactly this: "changes made from a HLOS aren't [persisted]" [25]) so bcdboot/Setup do not fail (failure mode INFERRED); rely on the removable-media path `\EFI\Boot\bootaa64.efi` that bcdboot also writes plus our auto boot options. **P2**: persist OS writes (e.g. journal in a reserved RAM page committed to `logfs` on the next boot - DDR survives warm reset, INFERRED; or TZ `uefisecapp` (`uefi_sec.mbn` is in the QCS6490 flat build) like Qualcomm WoA). Windows probably ignores EFI_RT_PROPERTIES_TABLE (INFERRED). |
| Runtime **ResetSystem** with reset *and* shutdown [3] | RUN | PSCI SYSTEM_RESET/OFF via SMC | OK (verify SYSTEM_OFF really powers off under Gunyah - INFERRED) |
| Runtime regions on AArch64 64 KiB aligned (UEFI AArch64 binding) | BOOT | `-z common-page-size=0x10000` for runtime drivers (QCS6490.dsc.inc:263-264) | OK; runtime services use no MMIO (PSCI/RAM only) - keep it so. |
| **GOP**: native/highest mode, `PixelsPerScanLine == HorizontalResolution`, `PixelBlueGreenRedReserved8BitPerColor`, physical FB, no BltOnly; "The frame buffer must continue to be scanned out after boot services have exited" [3] | BOOT (visible) / INST | Mode OK (Gop.c:212-215), FB in EfiReservedMemoryType below 4 GiB (MdssDisplayDxe.c:191-195, good), but **display stopped before EBS** (MdssDisplayDxe.c:140-160) | **P0**: for Windows keep DSI/LT9611/DPU running and keep the MDSS SMMU stream passable after EBS (Windows-only, via OS detection as Rockchip does, or an "ACPI mode" setting). BasicDisplay "inherits the linear frame buffer ... no mode or resolution changes are possible" [9]. |
| Interrupts masked at EBS; core system resources (GIC, timers, DMA) powered/clocked; pin-mux done by firmware; boot path devices clocked [3] | BOOT | ArmGicDxe masks at EBS; UFS/USB/PCIe left powered (no EBS handlers in Dwc3HostDxe, PCIe lib, RenesasXhciFwDxe) | OK, keep. |
| Debug port powered, clocked and initialised before hand-off ("serial, USB host/function, PCI ethernet") [3] | CERT | uart5 alive (XBL) | OK for serial; usb_1 needs bring-up for KDNET-EEM. |
| Block I/O + Device Path for ESP and OS volume [3]; GPT, ESP mandatory [3] | INST | UFS + USB BlockIo | OK |
| **EFI_RNG_PROTOCOL** "MANDATORY" (Req 26) and "required by Windows on SoC platforms" [3][7] | CERT (boot works without - INFERRED) | none | **P1**: RngDxe over an RngLib reading the SC7280 TRNG (0x010D3000; Linux qcom-rng reads `PRNG_DATA_OUT` @0x0). Qualcomm's own QCS6490 UEFI ships RngDxe (`windows-research/q6a-fw/inventory.md` FV3 #39). |
| EFI_HASH protocol, MOR (MemoryOverwriteRequestControl), TrEE/TCG protocol, HSTI [3] | CERT | none | Later / optional. |
| Secure Boot (Reqs 3-21) [3]; Win11: "must ship with UEFI Secure Boot enabled" [12 §3.6.2] | CERT; Setup check (bypassable) | `AuthVariableLibNull`, no SecureBootConfigDxe | **P2**: AuthVariableLib + SecureBootVariableProvisionLib with Microsoft KEK/db/dbx. 2011 CAs expire 2026-06-24 (KEK), 2026-06-27 (UEFI CA), 2026-10-19 (Windows PCA) [19]: ship **both** 2011 and 2023 certs. Needs persistent *runtime* authenticated writes for dbx/db updates, i.e. after the variable P2 item. |
| Capsule / ESRT | optional | CapsuleRuntimeDxe + DxeCapsuleLibNull (QCS6490.dsc.inc:175, 249, 581) | Optional; Windows Update firmware delivery only. |
| Memory map | BOOT | carve-outs EfiReservedMemoryType (README "Memory map") | OK. The bootmgr AllocatePages(AllocateAddress 0x102000) failure is harmless (established). EFI_MEMORY_ATTRIBUTES_TABLE is needed for HVCI (edk2 default, keep). |

---------------------------------------------------------------------------------------------------

## 3. SMBIOS [3][11][12]

- Windows needs SMBIOS >= 2.4 with the spec's required structures [3]; we publish 3.7 (QCS6490.dsc.inc:476), 64-bit entry point - OK.
- CHIDs are SHA-1 hashes of Type 0/1/2 fields (Manufacturer, Family, Product Name, SKU, Baseboard product, BIOS vendor/version/release) and are used for driver targeting [11]. Fix:
  - Type 1 UUID is all zero (OemMiscLib.c:361-366) -> derive a stable per-board UUID (e.g. from the QFPROM/SMEM serial; INFERRED source). Duplicate/zero UUIDs break activation/MDM tooling (INFERRED).
  - Serial "SN0000", SKU "SK0000" (RubikPi3.dsc:96-97) -> real serial; Family "QCS6490" (QCS6490.dsc.inc:482) -> product line, e.g. "RUBIK Pi" per [11]'s Family guidance.
  - Enclosure: we report Embedded PC 0x22 (OemMiscLib.c:184-189); [12] lists Mini PC 0x23 for "Partner System without integrated display" - CERT only.

---------------------------------------------------------------------------------------------------

## 4. Install checks, supported CPUs, bypasses, media

**Hardware floor** (consumer Win11 [12][13]): 1 GHz 2 cores, ARM64 with `PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE`
(24H2+ kernels do not boot on ARMv8.0 [26]), 4 GB RAM, 64 GB storage, UEFI + Secure Boot capable, TPM 2.0,
DX12/WDDM 2.0, 720p >9" display. "Upon approval from Microsoft ... custom image" systems may ship without TPM [12 §3.6.1].

**CPU lists**: QCS6490 and QCM6490 are on the Windows 11 **24H2** supported Qualcomm list [16]; the 25H2 page lists
only Snapdragon X but says later processors meeting the principles "will be considered as supported" [16b];
QCS6490 is supported for **Windows 11 IoT Enterprise LTSC 2024, 24H2 and 25H2** [17] (Qualcomm has a Windows
IoT BSP for it, not public; the Q6A UEFI we have is DT-only: `q6a-fw/inventory.md`).

**Edition choice (recommendation)**:
- *Windows 11 IoT Enterprise LTSC 2024* (90-day eval at aka.ms/winioteval [18]): TPM **optional**, Secure Boot **optional**,
  2 GB RAM / 16 GB storage allowed [15]; Hyper-V on Arm64 supported from LTSC 2024 / 24H2 [18]. No bypass needed.
- *Consumer 25H2 ISO (what we have)*: needs Setup bypasses:
  - `HKLM\SYSTEM\Setup\LabConfig` DWORDs `BypassTPMCheck`, `BypassSecureBootCheck`, `BypassRAMCheck` (+ community
    `BypassStorageCheck`, `BypassCPUCheck`): not documented by Microsoft; Rufus injects the first three through an
    `autounattend.xml` **windowsPE** pass and `BypassNRO` in the specialize pass, with `arm64` arch support, and wraps
    `setup.exe` for builds >= 26000 (rufus `src/wue.c` [20]). This works for a USB clean install (WinPE reads it).
  - `HKLM\SYSTEM\Setup\MoSetup\AllowUpgradesWithUnsupportedTPMOrCPU=1` is the Microsoft-published key, but only for
    in-place upgrades and still needs TPM 1.2 [21] - irrelevant here.
- **Media**: `sources/install.wim` in this ISO is 7,324,830,264 bytes (> FAT32 4 GiB). Our firmware reads FAT/UDF/ext4 only
  (no NTFS/exFAT). Use FAT32 + `Dism /Split-Image ... /SWMFile:install.swm /FileSize:3800`; Setup installs from
  `install.swm` automatically [22]. Only `boot.wim` (704 MB) is read through UEFI; `install.wim` is read by WinPE.
- **Target disk**: UFS LUN0, GPT. UFS is 4K-native (README rawprogram `SECTOR_SIZE_IN_BYTES="4096"`, README.md:267):
  ESP must be FAT32 >= 300 MB on 4Kn, MSR 16 MB, Windows >= 20 GB NTFS [23]. Windows Setup will show the firmware LUNs
  as extra disks - user must pick LUN0 (INFERRED risk).

---------------------------------------------------------------------------------------------------

## 5. Exception level, hypervisor, Hyper-V

- **Windows kernel always runs at EL1** ("Windows still uses EL1 for the actual operating system/NT kernel by design",
  also with VHE/Hyper-V) [28].
- **Windows on Snapdragon (WoA)**: XBL starts UEFI at EL1 under QHEE; Windows boots at EL1; to get EL2 for Hyper-V/VBS,
  winload runs `OslpTcbLaunchPhase0`, loads `tcblaunch.exe` and issues QHEE `hyp_manager_launch` SMCs, with TrustZone's
  `mssecapp` checking Microsoft's signature ("Secure Launch"/DRTM) [29][30]; "Snapdragon-based compute platforms ... boot
  the UEFI in EL1 ... the Microsoft bootloader has to rely on some custom mechanism to take over EL2" [31].
  Snapdragon X2 changed this: EFI runs at EL2 [29]. Qualcomm WoA FADTs advertise Hypervisor Vendor "QCOM" (8250 FACP.dsl).
- **RUBIK Pi 3 under Gunyah (EL1 mode)**: Windows itself should run (same position as Linux under Gunyah; vGIC, PSCI via
  SMC, virtual counter as our UEFI already uses, QCS6490.dsc.inc:101). Hyper-V/VBS/HVCI/WSL2 need the Secure-Launch path;
  project notes say the tcblaunch route seen on the Radxa Q6A is not available on RUBIK's Gunyah (unverified, INFERRED)
  -> expect **no Hyper-V at EL1**; keep FADT Hypervisor Vendor = 0 so winload does not try it (INFERRED). EL1 is also the
  mode Qualcomm's own Windows driver stack assumes (PIL/hyp-assign via SCM; Q8B pack `qcscm`, `qcpil`, `qcsubsys`), but
  no QCS6490 drivers are available to us. Blocking issues at EL1: SMMU stream policy (Gunyah faults unmatched streams and
  turns BYPASS S2CRs into FAULT, SmmuDxe.c:10-13) and USB DMA above 4 GiB timing out under Gunyah (README "USB") - Windows
  inbox drivers will DMA anywhere in 8 GiB (mitigation INFERRED: `bcdedit /set {default} truncatememory 0x100000000`).
- **EL2 mode (TrustZone removes Gunyah)**: apps SMMU left in bypass (SmmuDxe.c:15-16), no stage 2, so inbox DMA just works;
  GICv3 + EL2 lets winload launch the Microsoft hypervisor directly (Hyper-V on Arm64 supported since LTSC 2024/24H2 [18];
  INFERRED that hvloader accepts our platform). Use the **early** Gunyah exit (DSP preload Disabled / `PcdGunyahLateExit=0`):
  winload probably checks CurrentEL before ExitBootServices (INFERRED), and Windows has no use for the preloaded DSPs.
- **Recommendation**: bring Windows up at **EL2** first (fewest moving parts: no SMMU/DMA-limit work, Hyper-V possible),
  then decide whether EL1 is worth it (only if Qualcomm Windows drivers needing QHEE/Gunyah ever become usable).

---------------------------------------------------------------------------------------------------

## 6. CPU, PSCI/SMCCC, GIC, timers, watchdog, SMMU, PCIe

| Item | Windows expectation | QCS6490 facts | Action |
|---|---|---|---|
| CPU bring-up | PSCI; the Microsoft parking protocol only for non-PSCI platforms, and "Platforms that do support PSCI must not use this protocol" [3] | PSCI 1.0 via SMC (kodiak.dtsi:866-868) | FADT PSCI_COMPLIANT, GICC parking version 0, parked address 0. |
| Idle | `_LPI` (FFH = PSCI CPU_SUSPEND) or a PEP | DT: OS-initiated hierarchy (kodiak.dtsi:440-500, 866-920) | Phase 1: none (WFI only). Later `_LPI` in ACPI0010/ACPI0007; INFERRED that platform-coordinated states are safer. |
| SMCCC | SMCCC 1.1 ARCH_FEATURES / workarounds (INFERRED Windows probes them) | TZ/Gunyah provide | nothing to do |
| GIC | GICv3 MADT GICC/GICD/GICR; MSI via ITS (+IORT) or GICv2m-style MSI frame | GIC-600 0x17A00000 / GICR 0x17A60000; ITS disabled; SETSPI frame 0x17A10000 | see 1.1 MADT. |
| Generic timer | GTDT; CNTFRQ_EL0 programmed (ntos returns 0xC0000182 if 0) | 19.2 MHz set by boot firmware (INFERRED, Linux works) | GTDT as in 1.1. |
| Watchdog | optional: GTDT SBSA watchdog or WDAT (ntos/winload load WDAT) | no SBSA WDT; APSS WDT owned by Gunyah (kodiak.dtsi:6897-6903); Qualcomm uses a CSRT HAL extension (ACPI\QCOM0604) | none. The UEFI boot watchdog (WatchdogTimerDxe) is disabled by the OS loader per UEFI spec. |
| SMMU | No inbox MMU-500 support assumed (INFERRED); Kernel DMA Protection only "for external PCIe capable ports" [12 §3.6.3] | MMU-500 @0x15000000 (kodiak.dtsi:6711-6712); SIDs: UFS 0x80 (SmmuDxe), usb_2 0xA0, PCIe0 0x1C00/0x1C01, MDSS 0x900/0x402 (project notes) | EL2: nothing (bypass). EL1: for Windows keep bypass-CB S2CRs for UFS, usb_2, PCIe0, MDSS after EBS (skip the restore in SmmuDxe for Windows). No IORT. |
| PCIe | ECAM via MCFG; `_OSC` native control [12 §3.6.3 "PCIe Native Control must be enabled"]; MSI or INTx | DWC non-ECAM, internal iMSI (SPIs 141-148) not usable by Windows | MCFG + NXPMX6 quirk + INTx (Rockchip pattern), or iATU CFG-shift (ECAM-like) programming (INFERRED alternative). |
| DMA coherency | `_CCA` per device | UFS, PCIe0, PCIe1, SDHC1 dma-coherent; usb_2 not | `_CCA 1/0` accordingly. |

---------------------------------------------------------------------------------------------------

## 7. Debugging Windows on this board

**Serial kernel debugging (kdcom, DBG2 type 0x8000).** Microsoft's DBG2 spec lists subtype 0x0011 "SDM845 with clock
rate of 1.8432 MHz" and 0x0013 "SDM845 with clock rate of 7.372 MHz" [2] (ACPICA names `ACPI_DBG2_SDM845_1_8432MHZ`,
`ACPI_DBG2_SDM845_7_372MHZ`, Linux include/acpi/actbl1.h:765, 767). These are the Qualcomm QUPv3 **GENI** serial engine
(SDM845 introduced it; SC7280 uart5 is `qcom,geni-debug-uart`, kodiak.dtsi:1457-1458). Verified in the 25H2 ARM64
`kdcom.dll` (boot.wim 2, `Windows/System32/kdcom.dll`): the serial subtype dispatch table at VA 0x1c0009000 (range check
`cmp w8,#0x15` at 0x1c00025c4) has handlers for 0x01, 0x03-0x06, 0x08-0x0A, 0x0C-0x0E, 0x10-0x13; **0x11 and 0x13 are
present**, 0x00/0x02/0x07/0x0B/0x0F(DCC)/0x14 are not (`win-reqs/kdcom-dbg2-subtypes.txt`). The 0x13 init routine
(0x1c00048b0) programs GENI registers (0x258 DMA_MODE_EN, 0x25C/0x268/0x26C/0x280/0x28C/0x2A4/0x2A8 UART config, 0x614/0x644
IRQ enables, 0x80C-0x814 watermarks, 0x630 S_CMD0) and the baud routine (0x1c0004c10) writes the clock divider to
GENI_SER_M_CLK_CFG 0x48 / S_CLK_CFG 0x4C, i.e. kd re-initialises the SE but **assumes the SE source clock** (1.8432 or
7.3728 MHz) and needs the UART GENI firmware already loaded (XBL's for uart5). Action: read GCC QUPV3_WRAP0_S5 RCG at UEFI
time and pick 0x11 or 0x13 (or set the RCG to 7.3728 MHz before hand-off) (INFERRED). Qualcomm's own SM8250 table uses
0x0011. Windows commands: `bcdedit /dbgsettings serial debugport:1 baudrate:115200`, `/debug on`, `/bootdebug {bootmgr} on`
(debugport = DBG2 entry index, INFERRED for ARM). The Linux SPCR/earlycon code has no GENI subtype (no hits in
drivers/acpi/spcr.c), so SPCR only matters to Windows EMS.

**KDNET.** Requirements: "a Synopsys USB 3.0 controller connected to an USB type C port", transport `kd_8003_5143.dll`
for ARM (= DBG2 Net 0x8003, subtype = Qualcomm PCI vendor 0x5143), ARM selects the DBG2 entry with `busparams=1|2|3`;
host IP 169.254.255.255 [10]; naming `kd_<PortType>_<Subtype>.dll` [10b]. `kd_8003_5143.dll` is in the 25H2 WinPE
(`fwgap/bootwim.list:23537`). Our USB-C port is usb_1, a DWC3 in device mode (0x0A600000) - the matching hardware; needs a
DBG2 0x8003/0x5143 entry (Qualcomm layout: two GAS 0x0A600000 sizes 0xFFFFF and 0x1000 + 148-byte OEM data whose format
is undocumented - copy/learn from `8250/src/DBG2.dsl`), a DSDT `URS0` device, and usb_1 powered/clocked/PHY up before
hand-off [3] (today untouched). PCI NICs: kdnet supports "Intel, Broadcom, Realtek, Atheros, Emulex, Mellanox and Cisco"
[10]; the AX88179 (USB) is not usable; a PCIe NIC would need PCIe1 (M.2, unsupported yet).

**Without a debugger**: serial boot log from UEFI only; Windows boot failures surface as bugcheck codes on the GOP frame
buffer (needs the display to stay on, see 2).

---------------------------------------------------------------------------------------------------

## 8. Prioritised work list (firmware side)

P0 - reach WinPE Setup (EL2 mode, consumer ISO with LabConfig or IoT LTSC):
1. ACPI producer: AcpiTableDxe + static tables (FADT, MADT, GTDT, DSDT minimal, DBG2) + Windows/Linux table-mode switch.
2. Windows-safe hand-off: display keeps scanning out after EBS (and its SMMU stream at EL1); no S2CR restore for Windows.
3. Runtime SetVariable succeeds (memory-backed) instead of EFI_WRITE_PROTECTED.
4. DSDT: CPUs, UFS (`QCOM24A5`), usb_2 xHCI (`PNP0D10`), debug UART.
P1 - install to UFS and boot it: PCIe0 in ACPI (MCFG + `_PRT` + NXPMX6 quirk) for the USB 3.0 ports/keyboard/Ethernet
chip, EFI_RNG_PROTOCOL (TRNG), SMBIOS UUID/serial, PPTT rev 1, BGRT, PMK8350 RTC GetTime.
P2 - quality/compliance: persistent runtime variables, Secure Boot with 2011+2023 Microsoft certs, `_LPI`, MSI frame,
KDNET-EEM on usb_1, EL1 mode support, IORT/CSRT only if Qualcomm drivers become usable, TPM (QTEE fTPM via QcTrEE,
inbox `QcTrEE_i.inf:38` binds ACPI\QCOMFFEC - availability on this TZ unknown).

---------------------------------------------------------------------------------------------------

## Sources

Microsoft
- [1] ACPI System Description Tables: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-system-description-tables
- [2] Debug Port Table 2 (DBG2): https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-debug-port-table
- [3] UEFI Requirements for Windows on SoC Platforms: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/uefi-requirements-that-apply-to-all-windows-platforms
- [4] SPCR: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/serial-port-console-redirection-table
- [5] Minimum UEFI Requirements for Windows on SoC Platforms: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/minimum-uefi-requirements-for-windows-on-soc-platforms
- [6] Hardware Requirements for SoC-based Platforms: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/hardware-requirements-for-soc-based-platforms
- [7] UEFI Entropy Gathering Protocol: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/uefi-entropy-gathering-protocol
- [9] Microsoft Basic Display Driver: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver
- [10] KDNET over USB EEM (Arm): https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-kernel-mode-debugging-over-usb-eem-arm-kdnet
- [10b] KDNET transport extensibility (kd_XXXX_YYYY naming): https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/how-to-develop-kdnet-extensibility-modules
- [11] SMBIOS: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/smbios
- [12] Minimum Hardware Requirements for Windows 11 (June 2021 PDF, linked from https://learn.microsoft.com/en-us/windows-hardware/design/minimum/minimum-hardware-requirements-overview): https://download.microsoft.com/download/7/8/8/788bf5ab-0751-4928-a22c-dffdc23c27f2/Minimum%20Hardware%20Requirements%20for%20Windows%2011.pdf (§3.5 processor, §3.6.1 TPM, §3.6.2 UEFI/Secure Boot, §3.6.3 memory security, §3.7 storage, §3.8 memory)
- [13] Windows 11 specifications: https://www.microsoft.com/en-us/windows/windows-11-specifications
- [15] Windows IoT Enterprise minimum system requirements: https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/system_requirements
- [16] Win11 24H2 supported Qualcomm processors: https://learn.microsoft.com/en-us/windows-hardware/design/minimum/supported/windows-11-24h2-supported-qualcomm-processors ; [16b] 25H2: https://learn.microsoft.com/en-us/windows-hardware/design/minimum/supported/windows-11-25h2-supported-qualcomm-processors
- [17] Windows 11 IoT Enterprise supported Qualcomm processors: https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/winiot_qualcomm_processors_2024
- [18] Windows IoT FAQ (Arm64, Hyper-V, evaluation): https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/faq
- [19] Secure Boot certificate expiry: https://support.microsoft.com/en-us/topic/when-secure-boot-certificates-expire-on-windows-devices-c83b6afd-a2b6-43c6-938e-57046c80c1c2 and https://techcommunity.microsoft.com/blog/windows-itpro-blog/act-now-secure-boot-certificates-expire-in-june-2026/4426856
- [22] Install Windows from a USB flash drive (split install.wim): https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/install-windows-from-a-usb-flash-drive?view=windows-11
- [23] UEFI/GPT partitions (ESP 300 MB on 4Kn): https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions?view=windows-11

Other
- [20] Rufus `src/wue.c`: https://github.com/pbatard/rufus/blob/master/src/wue.c
- [21] AllowUpgradesWithUnsupportedTPMOrCPU: https://www.bleepingcomputer.com/news/microsoft/microsoft-shares-windows-11-tpm-check-bypass-for-unsupported-pcs/
- [24] edk2-rk3588 (Windows 11 in ACPI mode): https://github.com/edk2-porting/edk2-rk3588 ; local tree `edk2-rockchip/Silicon/Rockchip/RK3588/{AcpiTables,Drivers/AcpiPlatformDxe}`
- [25] RPi NVRAM behaviour: https://github.com/tianocore/edk2-platforms/blob/master/Platform/RaspberryPi/RPi3/Readme.md
- [26] Win11 24H2 requires ARMv8.1: https://www.osnews.com/story/139585/first-generation-windows-on-arm-pcs-will-not-be-able-to-run-windows-11-24h2/
- [27] Arm BSA 1.0C (ECAM/MSI rules): https://documentation-service.arm.com/static/635fa581c7882d1f2d3415da
- [28] Windows ARM64 internals (kernel at EL1): https://connormcgarr.github.io/arm64-windows-internals-basics/
- [29] Qcom Secure Launch (sc7180 analysis, X2 note): https://github.com/TravMurav/Qcom-Secure-Launch/ ; [30] slbounce: https://github.com/TravMurav/slbounce
- [31] HvArm chapter 0 (Snapdragon UEFI at EL1): https://0xabe.io/hypervisor/arm/2026/03/01/HvArm-Chapter-0.html
- Qualcomm WoA reference ACPI (SM8250, decompiled): https://github.com/WOA-Project/acpi_oem_qcom_common/tree/main/8250/src (FACP, APIC, GTDT, DBG2, MCFG, IORT, CSRT, PPTT, TPM2 .dsl); AeoB dumps: https://github.com/alexVinarskis/qcom-aeob-dumps
- Local: Radxa Q8B driver pack INFs `<downloads>/dragon-q8b_win_driver_pack_v1.0.0/driver_pack/` (HalExtQCWdogTimer8280 ACPI\QCOM0604, qcsmmu8280 QCOM0609, qciommu QCOM068F, qcscm QCOM04DD, qcsecapp QCOM06E4, QcTrEE QCOM04DE); DT `devicetree/mainline/upstream/src/arm64/qcom/kodiak.dtsi`, `qcs6490-thundercomm-rubikpi3.dts`; Linux 7.0.2 `include/acpi/actbl1.h`, `actbl3.h`, `drivers/rtc/rtc-pm8xxx.c`.
