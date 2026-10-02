# Windows 11 on Arm (ARM64) on the RUBIK Pi 3: phased plan

Status 2026-10-02. This plan is built from the research reports in this directory and a spot check of the
firmware sources. Nothing in it has been tried on the board yet. **INFERRED** marks conclusions that no source
states and no hardware test confirms.

Path prefixes:
- `Q/` = `edk2-qualcomm/Silicon/Qualcomm/QCS6490/`
- `P/` = `edk2-qualcomm/Platform/Thundercomm/RubikPi3/`
- `RK/` = `edk2-rockchip/Silicon/Rockchip/`
- `L/` = the Linux 7.0.2 tree
- `K:` = `devicetree/mainline/upstream/src/arm64/qcom/kodiak.dtsi`

## 中文摘要

- 目标分两层：
  - **A 层**：只用 Windows 自带驱动。从 USB 安装到 UFS，HDMI（GOP 帧缓冲，1080p 固定）、UFS、全部 USB、有线网可用。
  - **B 层**：Qualcomm 驱动栈（GPU、DSP、GPIO/I2C/SPI、PEP 电源管理）。
- **A 层**（阶段 0–4）可行，估计 2.5–4 个月工作量。
- **B 层**（阶段 6）取决于驱动和 ACPI 表的来源与许可，属于研究项目。
- **阶段 1** 只做最小步骤，让 WinPE 安装程序内核跑起来：
  - ACPI 最小表集：FADT、MADT、GTDT、DSDT（只含 CPU 和调试串口）、DBG2；
  - ExitBootServices 时识别 Windows（winload/winresume），HDMI 不关，SMMU 条目不还原；
  - 首选 EL2：在 SEC 早退 Gunyah，并关闭 DSP 预加载；EL1 作为后备。
- 明确的阻碍：
  - LT9611 HDMI 不能用 Qualcomm 显示驱动（qcdx）加速，只有基本显示；
  - 板载 Wi-Fi（AP6256，SDIO）没有驱动；
  - PCIe 是非 ECAM 的 DesignWare 控制器，需要 iATU 移位模式或 NXPMX6 怪癖；
  - 没有 PEP，因此没有调频、温控和睡眠；
  - 没有 TPM；
  - EL2 下 TrustZone 不运行 DSP。
- 需要你决定的事项见第 7 节。

---------------------------------------------------------------------------------------------------

## 0. Inputs and their state

| Report | State | Used for |
|---|---|---|
| `win-requirements.md` | complete (287 lines) | Windows requirements, table contents, Setup bypasses, kd/DBG2 |
| `prior-art.md` + `prior-art-acpi/` | complete | Radxa Q6A, Mu-Silicium/Aloha, public SC7280 ACPI, licensing |
| `q6a-firmware.md` + `q6a-fw/` | complete | Qualcomm OS/EL selection logic, IORT data, Q8B hardware IDs |
| `firmware-gaps.md` + `fwgap/` | complete (568 lines) | file:line gap analysis and the designs this plan adopts |
| `board-peripherals.md` | complete | per-device driver paths and likelihood |
| `bootmgr-winload.md` | **incomplete** (95 lines; stops in §2, before its requirement table) | its TL;DR is used: fatal tables, FADT fields, EL handling, PMU risk |

**Facts this plan rests on**, each cited in the reports:
- **Fatal ACPI lookups.**
  - Both boot apps need `FACP`: bootmgr at `0x101af6cc`, winload at `0x18018d71c`.
  - winload also needs `APIC` (`0x18001381c`).
  - Every other table is optional for the boot apps.
  - The kernel needs a GTDT with its virtual-timer GSIV, `CNTFRQ_EL0 != 0` and PSCI major version 1 (`bootmgr-winload.md` §0).
- **EL1 and EL2 both work for the boot apps.** When started at EL2, bootmgr and winload drop themselves to EL1 and keep an EL2 stub (`bootmgr-winload.md` §0).
- **FADT hypervisor vendor ID.** It is read only from FADT revision ≥ 6, and it must never be `"MsHyperV"`. The kernel treats `"QCOM"` as "no hypervisor" (`HviIsAnyHypervisorPresent` `0x1405c6ed0`).
- **WinPE inbox drivers cover the boot path:**
  - `storufs` binds `ACPI\QCOM24A5` (storufs.inf:84);
  - `usbxhci` binds `PNP0D10`, `PNP0D15` and `PCI\CC_0C0330` (usbxhci.inf:38-40);
  - the AX88179 driver is inbox (`fwgap/bootwim.list`);
  - BasicDisplay uses the GOP framebuffer.
- **The Radxa Dragon Q6A (same die) runs Windows with no drivers installed:** "HDMI (GOP), UFS, eMMC, PCIe/NVMe, USB 2/3" (`prior-art.md` §3.1).

---------------------------------------------------------------------------------------------------

## 1. What "full Windows support" can realistically mean here

| Tier | Contents | Driver source | Feasibility |
|---|---|---|---|
| **A: inbox Windows** | install from USB, boot from UFS, HDMI 1080p60 framebuffer (no GPU), USB 2.0 + 2x USB 3.0, Gigabit Ethernet (AX88179B), NVMe in M.2 (after PCIe1), correct time, reboot/shutdown | Windows' own drivers only, plus firmware work | **High**, Phases 0-5 |
| **B: Qualcomm stack** | GPU/video (qcdx), DSPs, GPIO/I2C/SPI/UART on the 40-pin header, PEP (DVFS, thermal, idle), USB-C | WOA-Project 7280 Windows Update mirror, or the Radxa Q6A 6490 pack; both are user-supplied | **Low-Med**, Phase 6, decision-gated |
| **C: custom drivers** | ES8316 headphone jack, HDMI audio, on-board Bluetooth, LT9611 re-lock helper | to be written (CoolStar-style) | **Low** |
| none | on-board Wi-Fi (AP6256 / BCM43456 over SDIO) | no ARM64 driver exists | **None**: use USB or M.2 Wi-Fi |

The phases target Tier A first. Tier A is the realistic meaning of "full support" for this firmware; Tier B
depends on decisions D7 and D8 (§7).

---------------------------------------------------------------------------------------------------

## 2. Conflicts between the reports, resolved

| Topic | What the reports say | Plan choice |
|---|---|---|
| FADT Hypervisor Vendor Identity | `win-requirements` §1.1: 0. `bootmgr-winload` §0: "QCOM", never "MsHyperV". `firmware-gaps` §2.2.4: "QCOM" only at EL1 | 0 at EL2 and "QCOM" at EL1. AcpiPlatformDxe patches it from the EL that SEC decided (Qcs6490NvStatusLib). Never "MsHyperV" |
| FADT `LOW_POWER_S0_IDLE` | `bootmgr-winload` minimal set: on. `win-requirements` and `firmware-gaps`: off until a PEP exists | **Off.** There is no PEP, so no Modern Standby. A/B test only if Windows rejects the FADT (**INFERRED**: RK3588 Windows runs with it off, `RK/RK3588/AcpiTables/Fadt.aslc:64`) |
| MADT efficiency classes | 0/1/2 (`win-requirements`) vs 0/1, as Qualcomm's SC7280 MADT has (`firmware-gaps`) | 0 for cpu0-3, 1 for cpu4-7, as Qualcomm does. Try 0/1/2 later as an experiment |
| EL for Windows | `win-requirements` and `firmware-gaps`: EL2. `prior-art` and `q6a-firmware`: Qualcomm's design is EL1 under Gunyah | **EL2 is primary for Tier A.** EL1 is built in Phase 1-2 as the fallback, and is required for Tier B (TrustZone runs DSPs only for a Gunyah guest, README "DSPs at EL2") |
| UFS `_CCA` | DT `dma-coherent`, so `_CCA 1` (`win-requirements`). `firmware-gaps`: `_CCA 0` first | `_CCA 0` first (UEFI's proven non-coherent path); try 1 later |
| usb_2 IDs | Qualcomm: `QCOM0AA1` / `_CID PNP0D15`. RK: `PNP0D10` | **`_HID PNP0D10`** while Tier A, so that Windows Update cannot match a Qualcomm `QcXhciFilter` onto it (that filter breaks USB boot on the Q6A, `q6a-firmware.md` §6). Switch to Qualcomm IDs in Tier B (**INFERRED** risk) |
| GTDT platform (memory-mapped) timer | include 0x17C20000 (`win-requirements`) vs leave out at EL1 (`firmware-gaps`) | Omit in Phase 1; add at EL2 later if Windows wants it |
| UFS0 MMIO length | 0x3000 (DT) vs Qualcomm's 0x1C000, which includes ICE | 0x3000. If storufs touches ICE, use 0x1C000 (**INFERRED**) |

---------------------------------------------------------------------------------------------------

## 3. Cross-cutting firmware architecture (built in Phase 1, extended later)

New and changed modules:

```
Q/AcpiTables/                      (new) Fadt.aslc Madt.aslc Gtdt.aslc Dbg2.aslc Spcr.aslc Mcfg.aslc Pptt.aslc
                                         Cpu.asl Uart.asl Ufs.asl Usb2.asl Pcie0.asl Common.asl AcpiTables.h
P/AcpiTables/AcpiTables.inf        (new) FILE_GUID 7E374E25-8E01-4FEE-87F2-390C23C606CD, Dsdt.asl (#includes)
Q/Drivers/AcpiPlatformDxe/         (new) gate on ConfigTableMode; install at EndOfDxe (EmbeddedPkg AcpiLib
                                         LocateAndInstallAcpiFromFv[Conditional]); patch FADT/_STA; EBS patches
Q/Drivers/HandoffDxe/              (new; absorbs GunyahExitDxe) the only gBS->ExitBootServices wrapper;
                                         OS identification; QCS6490_HANDOFF_PROTOCOL { GetOsType, RegisterHandler }
Q/Drivers/PlatformConfigDxe/       (+) ConfigTableMode (ACPI / DT / ACPI+DT; default ACPI+DT); later "OS profile"
Q/Drivers/MdssDisplayDxe/          (+) "Display at OS handoff": Auto / Keep / Off
Q/Drivers/SmmuDxe/                 (+) RestoreSmmu skipped for Windows at EL1 (keep UFS/MDSS/USB2/PCIE0)
Q/Drivers/NvStoreFvbDxe/           (+) runtime RAM mode for Windows boots (Phase 2)
Q/QCS6490.dsc.inc, Q/QCS6490.fdf   (+) AcpiTableDxe, AcpiLib, PcdInstallAcpiSdtProtocol=TRUE, new INFs
```

The FDF already includes `ArmVirtPkg/ArmVirtRules.fdf.inc` (`Q/QCS6490.fdf:281`), which provides the `ACPITABLE`
rule. The shell already links `acpiview` (`Q/QCS6490.dsc.inc:516`). About 1.4 MiB of compressed FV space is free
(`firmware-gaps.md` §1).

**HandoffDxe behaviour.** It is installed at DXE time and always wraps EBS. RK's hook restores the original pointer
before calling it (`RK/Drivers/ExitBootServicesHookDxe/ExitBootServicesHookDxe.c:131-134`), which would break a
second wrapper, so there must be only one. GunyahExitDxe's wrapper (`Q/Drivers/GunyahExitDxe/GunyahExitDxe.c:269-289,356-361`)
moves into HandoffDxe.

The caller is identified as in RK (`OsIdentification.c`), with one extension: Windows is matched by a PDB path
containing `winload` **or `winresume`**. RK matches only `winload` (`OsIdentification.c:41`).

| Action at ExitBootServices | Linux (arm64 Image magic) | Windows (winload / winresume) | Unknown |
|---|---|---|---|
| Display (`Q/Drivers/MdssDisplayDxe/MdssDisplayDxe.c:141-160`) | stop (today) | **keep scanning out** | stop (today) |
| SMMU restore at EL1 (`Q/Drivers/SmmuDxe/SmmuDxe.c:255-269,714-724`) | restore (today) | **keep** 0x80, 0x900/0x402, 0xA0, 0x1C00/1 | restore |
| Deferred (late) Gunyah exit | exit (today) | **do not exit**: stay at EL1, keep SMMU, warn on serial | exit (today) |
| Runtime SetVariable (`Q/Drivers/NvStoreFvbDxe/NvStoreFvbDxe.c:1656-1660,1716-1718`) | refuse (today) | accept into RAM (Phase 2) | refuse |
| PMIC RTC runtime services | disabled, so rtc-pm8xxx owns the RTC (Phase 4) | enabled | enabled |
| ACPI quirk patches (NXPMX6 etc.) | none | if needed (Phase 3) | none |

Why the late exit is refused under winload: winload started at EL1, and switching it to EL2 in the middle of the
handoff would leave its EL1 system-register setup for the kernel in the wrong regime (**INFERRED**, high risk,
`firmware-gaps.md` §2.2.3). Linux tolerates the switch because the kernel checks CurrentEL at entry.

---------------------------------------------------------------------------------------------------

## 4. Phases

Effort is in engineer-days (ED) for someone who knows this tree, board time included. The ranges are wide
because none of the hardware behaviour has been tested.

### Phase 0: Preparation and cheap experiments (no Windows code path yet)

**Goal:** answer the hardware questions that pick between designs, and prepare install media.

| # | Item | How | Decides |
|---|---|---|---|
| 0.1 | uart5 GENI clock | UEFI shell `mm` on the GCC QUPV3_WRAP0_S5 RCG and SE5 `GENI_SER_M_CLK_CFG` (0x994048) | DBG2/SPCR subtype 0x11 (1.8432 MHz) vs 0x13 (7.3728 MHz) (`win-requirements.md` §7) |
| 0.2 | Scan-out after EBS | Small test EFI app (workspace only): GOP fill, `ExitBootServices`, then draw into the FB and spin. Firmware built with a temporary "skip DisplayStop / skip RestoreSmmu" flag. Run at EL1 and at EL2 | Shows the display-keep design works before ACPI exists |
| 0.3 | DMA above 4 GiB for UFS and usb_2, at EL1 and at EL2 | Temporary build that lets QcomUfsHcDxe/XhciDxe use buffers above 4 GiB (today bounced below 4 GiB, `Q/Drivers/Dwc3HostDxe/Dwc3HostDxe.c:27-36`) | Whether EL1 Windows needs `truncatememory`; supports the EL2 recommendation |
| 0.4 | PCIe0 ECAM shape | Shell `mm`: program iATU CFG-shift regions, then read 0x60100000 (expect VID 0x1912), 0x60101000 and 0x60108000 (expect all ones), 0x60008000 and 0x600F8000 (bus-0 aliasing / SError) (`firmware-gaps.md` §5.4) | Plain MCFG vs NXPMX6 vs SINGLE_DEV (Phase 3) |
| 0.5 | INTx delivery | Linux mainline `pci=nomsi`, then check `/proc/interrupts` for SPI 149 on xhci 01:00.0 | Whether `_PRT` INTx works (Phase 3) |
| 0.6 | Warm-reset RAM retention | Pattern at 0xA0000000, PSCI SYSTEM_RESET2 warm, check in SEC | Variable carry-over (Phase 4) |
| 0.7 | Finish `bootmgr-winload.md` | Use the existing `dis/` output. Settle: the winload EL2 path (`OslArchMicrovisorSetup`, TcbLaunch / `Qualcomm.dll` strings in `dis/winload.strings`), PMU accesses, required DSDT objects | Phase 1 risk list |
| 0.8 | Install media | 25H2 ARM64 ISO onto a FAT32 stick. Split `install.wim` (7.3 GB) with open-source `wimlib-imagex split … install.swm 3800`, or `Dism /Split-Image`. Add an `autounattend.xml` windowsPE pass with LabConfig `BypassTPMCheck` / `BypassSecureBootCheck` / `BypassRAMCheck` (Rufus `src/wue.c` pattern), or use IoT Enterprise LTSC 2024 (decision D1). Stick stays out of git | Phase 1/2 media |

- **Dependencies:** none.
- **Effort:** 3-6 ED.
- **Risk:** low. A test build could fault under Gunyah (0.3), but recovery is a reset.

### Phase 1: Windows installer kernel running (smallest step)

**Goal:** USB stick on the USB 2.0 port → `bootaa64.efi` → bootmgr → winload → ntoskrnl from the `boot.wim` RAM
disk → the first WinPE "Windows Setup" screen on HDMI.

No storage or USB driver is needed in Windows for this: WinPE runs from the RAM disk that bootmgr reads through
UEFI BlockIo (`bootmgr-winload.md` §0). Input does not work yet.

**Work items:**
1. **ACPI plumbing** (§3):
   - `MdeModulePkg/Universal/Acpi/AcpiTableDxe` and `AcpiLib|EmbeddedPkg/Library/AcpiLib/AcpiLib.inf`;
   - `PcdInstallAcpiSdtProtocol=TRUE`;
   - no RSDT, only RSDP rev 2 + XSDT, which bootmgr requires (`bootmgr-winload.md` §0).
2. **Tables** (`Q/AcpiTables/`). Values come from `K:` as tabulated in `win-requirements.md` §1.1 and `firmware-gaps.md` §2.2.4:
   - **FADT** rev 6.x:
     - `HW_REDUCED_ACPI`; no `LOW_POWER_S0_IDLE`;
     - ArmBootArch `PSCI_COMPLIANT` with no `PSCI_USE_HVC` (the conduit is SMC under Gunyah too, `K:866-868`);
     - HV vendor ID per §2;
     - OEM ID/table ID chosen by us (not "QCOM", see D8).
   - **MADT:**
     - 8 GICC entries: UID 0-7, MPIDR 0x000-0x700, PMU GSIV 23, VGIC maintenance GSIV 25;
     - GICD 0x17A00000 v3; GICR range 0x17A60000/0x100000;
     - no ITS, no MSI frame.
   - **GTDT:** PPIs 29/30/27/26; CntControlBase/CntReadBase = ~0; no platform timer and no watchdog.
   - **DBG2 + SPCR:** GENI 0x8000 with subtype 0x11 or 0x13 (from 0.1), base 0x00994000, GSIV 638, namespace `\_SB.UAR5`.
   - **DSDT:**
     - `\_SB.CPU0..7` (`ACPI0007`, `_UID` = MADT UID), optionally inside an `ACPI0010` container;
     - `\_SB.UAR5`, with a `_HID` that no inbox driver binds;
     - `\_SB._OSC` granting nothing.
   - Pattern files: `RK/RK3588/AcpiTables/{Fadt,Madt,Gtdt,Dbg2,Spcr}.aslc`, `Cpu.asl`.
3. **`Q/Drivers/AcpiPlatformDxe`:**
   - gated on `ConfigTableMode & ACPI`, as `RK/RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c:573-576` does;
   - installs the tables at EndOfDxe (`:150-205`);
   - patches the FADT HV ID from the EL decision.
4. **`ConfigTableMode`** in PlatformConfigDxe (VFR, en + zh-Hans strings), default **ACPI+DT**:
   - DT installation is gated on the DT bit: replace or wrap the stock `EmbeddedPkg/Drivers/DtPlatformDxe` (`Q/QCS6490.fdf:165`, `Q/QCS6490.dsc.inc:641`) and hide its own DtAcpiPref form, so there are not two switches.
   - Linux keeps using DT while both are installed: arm64 enables ACPI only with `acpi=` or a stub DT (`L/arch/arm64/kernel/acpi.c:198-209`).
5. **`Q/Drivers/HandoffDxe`** (§3): OS identification; the late exit is refused under Windows; the handler registry.
6. **Display Keep:** if `GetOsType() == Windows`, `OnBeforeExitBootServices` (`MdssDisplayDxe.c:151-158`) returns without `DisplayStop()`, and the MDSS interrupt enables are masked. Already correct and untouched:
   - the framebuffer is 1920x1080 BGRX, `EfiReservedMemoryType` below 4 GiB (`MdssDisplayDxe.c:177-219`);
   - the GOP PixelFormat is 1 (BGRX), the only format winload accepts (`bootmgr-winload.md` §0).
7. **EL/SMMU decision:**
   - **Primary: EL2.** Hypervisor setting = EL2 and DSP Preload = Disabled gives the early SEC exit (`Q/Library/Qcs6490Lib/EarlyInit.c`; README "Exception level"). The SMMU is then in bypass and nothing needs to be kept.
   - **Fallback: EL1.** SmmuDxe's `RestoreSmmu` skips all `mStreams` entries for Windows (`SmmuDxe.c:100-105,255-269`). Phase 1 itself only needs MDSS; keeping all four costs nothing.
   - **Misconfiguration** (late exit pending + Windows): stay at EL1 with the entries kept, and print a warning.

**Dependencies:**
- 0.1 (DBG2 subtype), 0.2 (display keep), 0.8 (media).
- No Qualcomm or Microsoft binaries enter the build.

**Verification on the board:**
1. UEFI shell: `acpiview -l` lists RSDP/XSDT/FACP/APIC/GTDT/DSDT/DBG2/SPCR, and `acpiview` shows no checksum or field errors.
2. **Linux regressions** (both must be unchanged; DT is used, and the HDMI teardown still happens for Linux):
   - Thundercomm 6.6 at EL1 (ADSP/CDSP/video/Wi-Fi up);
   - mainline 7.0.2 at EL2 (KVM, DSP preload attach).
3. Optional smoke test (**INFERRED** feasible): mainline Linux at EL2 with `acpi=force` and Display = Keep forced. Expect 8 CPUs through PSCI, the arch timer from the GTDT, GICv3 from the MADT, and an efifb console. This validates the MADT, GTDT and FADT without Windows.
4. **Windows, at EL2:**
   - the stick boots and the Windows boot logo appears;
   - then the WinPE Setup language screen shows on HDMI.
   - Debugging:
     - edit the stick's BCD on a PC: `bcdedit /store <stick>\efi\microsoft\boot\bcd /set {default} debug on`, then `/dbgsettings serial debugport:1 baudrate:115200` (debugport numbering on ARM is **INFERRED**), and `/set {bootmgr} bootdebug on`;
     - connect WinDbg through the board's Micro-USB debug UART;
     - bugcheck codes show on HDMI (Display Keep).
5. **Windows, at EL1:** the same test. A documented failure is acceptable here.

- **Exit criterion:** the WinPE Setup screen is visible at EL2.
- **Effort:** 10-18 ED: tables and plumbing 5-8; HandoffDxe and display/SMMU 3-5; board iterations 2-5.

**Risks and fallbacks:**
- **ACPI_BIOS_ERROR (0xA5) or HAL bugcheck (0x5C).** Check with kd; compare our tables field by field against the RK3588 tables and the public SC7280 dumps (`prior-art-acpi/`, reference only).
- **PMU access trapped by Gunyah at EL1.** bootmgr reads `PMCCNTR_EL0` right after the FADT check (`0x101af7c0`), and winload writes `PMCR_EL0` (`bootmgr-winload.md` §0). This is the reason EL2 is primary. **INFERRED** low risk, since Linux uses the PMU under Gunyah.
- **winload's EL2 path** sets up its "microvisor". With HV ID 0 and WinPE not launching Hyper-V, this is expected to work, as on RK3588 and RPi (**INFERRED**). Fallback: EL1.
- **Display goes black at the kernel's display handoff.** BasicDisplay needs the framebuffer winload records. Fallback: kd only, while debugging.
- **Gunyah virtual watchdog at EL1.** **INFERRED** low risk: it stays unarmed unless the guest enables it, and UEFI sits in setup at EL1 indefinitely without a reset.

### Phase 2: Install to UFS and boot Windows from UFS (inbox drivers, USB 2.0 port only)

**Goal:** complete Setup from the USB 2.0 port (stick, keyboard and mouse on a hub), reboot into Windows on UFS
LUN 0, finish OOBE, and reach a desktop on the BasicDisplay framebuffer.

**Work items:**
1. **DSDT `UFS0`:**
   - `_HID QCOM24A5`, `_CLS {0x01,0x09,0x01}`;
   - Memory32Fixed 0x01D84000/0x3000, Interrupt Level/High **297** (`K:2471-2487`);
   - `_CCA 0`;
   - no `_DEP`.
   storufs binds this with its Qualcomm install section (`fwgap/2/Windows/INF/storufs.inf:84,352-360`).
2. **DSDT `XHC0`** (usb_2):
   - `_HID PNP0D10` (§2), Memory32Fixed 0x08C00000/0x100000, GSIV **274**, `_CCA 0`;
   - `RHUB.PRT1` `_UPC`/`_PLD` (Type-A);
   - pattern: `RK/RK3588/AcpiTables/Usb3Host0.asl`.
3. **AcpiPlatformDxe `_STA` patching:** hide `UFS0`/`XHC0` when QcomUfsHcDxe or Dwc3HostDxe failed (`firmware-gaps.md` §2.2.2).
4. **NvStoreFvbDxe "runtime RAM mode", enabled by a HandoffDxe handler only for Windows:**
   - runtime Write/Erase update `mNvStore` and a dirty bitmap instead of returning `EFI_WRITE_PROTECTED`;
   - the SetVariable bits are set in the RT properties table (`NvStoreFvbDxe.c:1773-1810`);
   - pattern: `RK/Drivers/RkFvbDxe/RkFvbDxe.c:3-4,770-777`;
   - the variable driver does not reclaim at runtime, so ensure free space before handoff (**INFERRED**).
5. **BDS creates "Windows Boot Manager" itself:**
   - fork `ArmPkg/Library/PlatformBootManagerLib` (`Q/QCS6490.dsc.inc:177`) or add a hook;
   - scan fixed-media ESPs for `\EFI\Microsoft\Boot\bootmgfw.efi`;
   - add a Boot#### entry and put it first unless the user has reordered.

   This entry is written at boot time, so it persists. It also stops Setup's reboot from re-entering the USB installer.
6. **HandoffDxe at EL1:** keep `UFS0`/`XHC0` SMMU entries (already done in Phase 1). If 0.3 showed >4 GiB DMA failing, document `bcdedit /set {default} truncatememory 0x100000000` for EL1.
7. **Documentation** (README "Windows" section):
   - media (0.8);
   - which UFS disk to pick: storufs shows **every LUN** as a disk; LUN 1/2/4/5 hold boot firmware, and LUN 4 holds `uefi_a` and our `logfs` variables;
   - turn off Fast Startup and hibernation (`powercfg /h off`) until Phase 4;
   - turn off VBS/HVCI auto-launch at EL2 if it misbehaves (`bcdedit /set hypervisorlaunchtype off`).

**Dependencies:**
- Phase 1.
- Decision D6 (disk layout: flange Linux currently owns LUN 0).
- No Qualcomm drivers.

**Verification:**
1. Setup lists the UFS LUNs, and the keyboard works.
2. The install to LUN 0 finishes, and the reboots go through our BDS entry.
3. OOBE finishes with a local account (BypassNRO, or the IoT Enterprise edition). There is no network yet unless a USB Ethernet dongle (inbox RTL8153 driver) is on the hub.
4. Device Manager shows the Qualcomm UFS host controller and the xHCI host with no error codes.
5. Data integrity: copy and hash 10 GB on UFS; run `chkdsk`.
6. 20 reboot cycles. Start → Shut down really powers off (PSCI SYSTEM_OFF).
7. Both Linux kernels still boot (GRUB) after Windows is installed.
8. `bcdedit /enum firmware` runs (runtime GetVariable).

- **Effort:** 8-15 ED.

**Risks and fallbacks:**
- **storufs fails to bring up the link or high-speed gear.** The Q6A shows the inbox driver works on this die when firmware leaves UFS initialised. Fallbacks: try a 0x1C000 MMIO length, `_CCA 1`, or the generic `ACPI\CC_010901` binding.
- **Setup still fails at bcdboot.** Fallback: an unattend `bcdboot … /s` step, which relies only on `\EFI\BOOT\BOOTAA64.EFI` plus our BDS entry.
- **The wrong LUN is picked.** That is not a brick: the boot chain can be recovered through EDL. The risk is documented.
- **VBS/HVCI is enabled by default on a clean install at EL2** and the hypervisor fails on this platform (**INFERRED**). Fallback: `hypervisorlaunchtype off`.

### Phase 3: PCIe0 in Windows (2x USB 3.0, on-board Gigabit Ethernet)

**Goal:** Windows enumerates the Renesas uPD720201 exactly once, with working SuperSpeed ports and the AX88179B
Ethernet (inbox `netax88179_178a` / `usbncm`).

**Work items:**
1. **ECAM-shaped iATU** in `Q/Library/Qcs6490PciHostBridgeLib/Pcie0Init.c`:
   - region 0: CFG0, shift mode, 0x60100000-0x60107FFF (01:00.x);
   - region 2: CFG1, shift mode, 0x60108000-0x601FFFFF, so the endpoint answers UR instead of appearing 32 times;
   - optionally move the iATU registers out of the window (`PARF_ATU_BASE_ADDR`, `Pcie0Init.c:541`).
   Linux's DWC ECAM mode is the model (`L/drivers/pci/controller/dwc/pcie-designware-host.c:420-459`).
2. **`Q/Library/Qcs6490PciSegmentLib`** becomes plain ECAM (`0x60000000 + (B<<20|D<<15|F<<12|R)`). It keeps today's guards: bus 0 slot > 0 reads all ones, as does link down.
3. **MCFG:** seg 0, base 0x60000000, buses 0-1. This is Qualcomm's own SC7280 Windows layout (`prior-art-acpi/…/MCFG.dsl:25-32`).
4. **DSDT `PCI0`:**
   - `PNP0A08`/`PNP0A03`, `_SEG 0`, `_BBN 0`, `_CCA 0`;
   - `_CRS`: bus 0-1, MEM 0x60300000-0x63FFFFFF, no IO;
   - `_PRT` INTA-D → GSIV 181-184 level-high (`K:2249-2252`);
   - `_OSC` mask from `RK/RK3588/AcpiTables/Pcie.asl:93-96`;
   - `RES0` (PNP0C02): 0x60000000/0x200000, PARF 0x01C00000, PHY 0x01C06000;
   - no `_PR3`, so no D3cold;
   - `_STA` from a new `PcdPcie0LinkUp`.
5. **FADT `IaPcBootArch = MSI_NOT_SUPPORTED`** while INTx is used (`RK/RK3588/AcpiTables/Fadt.aslc:61`).
6. **Fallback quirk path,** applied by AcpiPlatformDxe at EBS for Windows only: FADT OEM ID "NXPMX6" with split MCFG, or SINGLE_DEV (`RK/RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c:207-392`).

**Dependencies:**
- Phase 2; experiments 0.4 and 0.5.
- The Renesas firmware is still loaded by `RenesasXhciFwDxe` from the `usb_fw` partition. That file is Renesas's, is read at run time and is never in the repo.

**Verification:**
1. UEFI: shell `pci` shows 00:00.0 and a single 01:00.0. USB 3 boot still works.
2. Linux regression at both ELs: Linux reprograms the iATU from its DT (`L/drivers/pci/controller/dwc/pcie-qcom.c:383-403`); check it still enumerates.
3. Windows:
   - a single xHCI appears;
   - a USB 3 stick reaches > 200 MB/s;
   - the AX88179B gets DHCP;
   - `iperf3` reaches about 900 Mbit/s;
   - hot-plug works on the USB 3 ports;
   - 20 reboot cycles;
   - the device stays up after disabling and re-enabling it in Device Manager.

- **Effort:** 6-15 ED.

**Risks and fallbacks:**
- **Bus 0 aliasing or SError on UR.** Fall back to NXPMX6 or SINGLE_DEV.
- **INTx is untested on this board** (Linux uses DWC iMSI). Fallback: the GIC MBI MSI frame in the MADT (0x17A10000, SPI base 832, count 128, the same as Qualcomm's MADT and dmesg "MBI range [832:959]"). Whether it works under Gunyah is **INFERRED**.
- **The Renesas firmware is lost** if Windows issues a secondary-bus reset or D3cold. The chip has no EEPROM, and there is no Windows ARM64 loader (**INFERRED**). Fallback: block D3 on the child device, or move the install stick and Ethernet to usb_2.

### Phase 4: Platform completeness for daily use (still inbox drivers)

| # | Item | Files / design | Verify | ED |
|---|---|---|---|---|
| 4.1 | EFI_RNG_PROTOCOL | `MdeModulePkg RngDxe` + new `Qcs6490TrngLib` reading the TRNG at 0x010D3000 (`K:2109-2112`). Replaces `BaseRngLibTimerLib` (`Q/QCS6490.dsc.inc:151`) | shell `rng`-style test app; Windows boots | 2-3 |
| 4.2 | Real time | new `Qcs6490PmicRtcLib` (RealTimeClockLib) for PMK8350 RTC 0x6100 over SPMI (arbiter 0x0C440000). Runtime MMIO. Disabled for Linux (pattern `RK/Drivers/RuntimeServicesManagerDxe/RuntimeServicesManagerDxe.c:62-90` + RT properties). Replaces `VirtualRealTimeClockLib` (`Q/QCS6490.dsc.inc:136`) | time survives power-off; Linux `hwclock` unaffected | 4-7 |
| 4.3 | SMBIOS identity | UUID and serial from SoC serial (SMEM socinfo, **INFERRED** source), SKU, Family "RUBIK Pi" (`Q/Library/OemMiscLib/OemMiscLib.c:361-366`, `P/RubikPi3.dsc:96-97`). Keeps CHIDs distinct from Qualcomm reference designs | `Win32_ComputerSystemProduct` | 1-2 |
| 4.4 | PPTT rev 1, BGRT, FPDT | `RK/RK3588/AcpiTables/Pptt.aslc` pattern (**rev 1**: Windows mis-parses rev 2/3); `BootGraphicsResourceTableDxe`; FirmwarePerformanceDxe | Task Manager shows 8 cores / 1 socket; logo persists | 2-3 |
| 4.5 | Fan and LED at handoff | fixed PWM on PM8350C LPG ch3 (gpio8) over SPMI; Windows has no fan control | 30-min CPU stress, compare with the Linux temperature | 2-4 |
| 4.6 | CPU performance level | without PEP the cores stay at the frequency XBL/UEFI left. Pin EPSS (0x18591000) to a chosen level before EBS (**INFERRED**). Research: ACPI `_CPC` on EPSS | benchmark vs Linux | 3-10 |
| 4.7a | Variables survive a warm reset | dirty bitmap + CRC in the status page (`Q/Include/Qcs6490NvStore.h`), `EfiResetWarm`/`Cold` as PSCI SYSTEM_RESET2 warm, SEC adopts the RAM copy (needs 0.6) | `bcdedit /set {fwbootmgr} …` survives a restart | 4-6 |
| 4.7b | Variables survive shutdown | runtime UFS write path (extend `Q/Library/Qcs6490Lib/EarlyUfs.c` with WRITE(10)/SYNC CACHE) called from ResetSystem | same, after shutdown | 6-12 (risky) |
| 4.8 | Secure Boot | RK `SECURE_BOOT_ENABLE` plumbing (`RK/Rockchip.dsc.inc:143-155,510-517`): AuthVariableLib, SecureBootConfigDxe, default keys. **Microsoft KEK/db 2011 + 2023 and dbx supplied at build time from outside the repo**. Needs 4.7 for runtime dbx updates | `Confirm-SecureBootUEFI` = True | 4-6 |
| 4.9 | Fast Startup / hibernation | HandoffDxe already matches `winresume`; stabilise runtime memory placement (`PcdPrePiProduceMemoryTypeInformationHob`) | hibernate and resume 10 times | 2-4 |
| 4.10 | EL1 parity (if D2 keeps EL1) | FADT "QCOM", SMMU keep list, `truncatememory` if needed | Phases 1-3 tests at EL1 | 3-6 |
| 4.11 | Hyper-V at EL2 | test `hypervisorlaunchtype auto`, WSL2, Hyper-V VM (IoT LTSC 2024 supports Arm64 Hyper-V) | VM boots | 2-5 |

- **Dependencies:** Phase 2 (4.1-4.6 can run alongside Phase 3).
- **Effort:** 30-60 ED in total.
- **Risks:**
  - TRNG access rights at EL1 (**INFERRED**).
  - SPMI ownership conflicts at runtime. Windows has no SPMI driver in Tier A, so none are expected (**INFERRED**).
  - Runtime UFS writes racing storufs. They are called only from ResetSystem after Windows has quiesced storage (**INFERRED**). Fallback: stop at 4.7a.

### Phase 5: More hardware with inbox or third-party drivers

| # | Item | Work | Likelihood | ED |
|---|---|---|---|---|
| 5.1 | M.2 Key M NVMe (PCIe1) | PCIe1 bring-up in UEFI (PERST gpio2, WAKE gpio3, power gpio56; DBI 0x40000000), then MCFG seg 1 0x40000000 bus 0-1 (as Qualcomm does) + `PCI1` like 3.4. Inbox `stornvme`. Also a clean Windows-only disk (solves D6) | Med-High | 10-20 |
| 5.2 | Wi-Fi replacement | USB Wi-Fi with an ARM64 driver, or an M.2 E-key card through an adapter on PCIe1 (a Qualcomm WCN685x driver exists in the 8280 pack; MSI needs are **INFERRED**) | Med | 2-5 |
| 5.3 | USB-C (usb_1) | host-only USB 2/3 forced in UEFI (orientation gpio140, SBU mux gpio52/53) described as `PNP0D10`; or device mode for KDNET-EEM (DBG2 0x8003/0x5143, `kd_8003_5143.dll` is inbox). The port is also the power input (PD dock needed) | Low-Med | 10-20 |
| 5.4 | Thermal reporting | ACPI thermal zones reading TSENS (0x0C263000/0x0C265000) through a SystemMemory OperationRegion (**INFERRED**) | Low-Med | 5-10 |
| 5.5 | Bluetooth (BCM4345C5 on uart7) | needs a GENI SerCx2 UART driver (Qualcomm `qcuart` or our own) plus the WoR `cywbtserialbus` (licensing unclear) | Low | 10-20 |

### Phase 6: Qualcomm Windows driver stack (Tier B, decision-gated research)

**Goal:**
- GPU and video acceleration (qcdx);
- DSP subsystems;
- GPIO/I2C/SPI/UART on the 40-pin header;
- PEP: DVFS, thermal, idle and Modern Standby.

**Hard prerequisites:**
- **EL1 under Gunyah.** TrustZone runs the DSPs only for a Gunyah guest (README "DSPs at EL2"), and Qualcomm's design keeps Gunyah for Windows (`q6a-firmware.md` §3.1).
- **One consistent driver set.** Mixing sets risks bugcheck 0x14E from PEP (`prior-art.md` §3.1). The candidates:
  1. WOA-Project mirror of the 7280 Windows Update CLS set (production-signed, `QCOM0Axx` HIDs, `SUBSYS_*7280`);
  2. Radxa Q6A `Q6A_WoS_DriverPackage_251205_testsigned.7z` (QCS6490-specific, e.g. GPU `QCOM0E36`, `qcpep6490`; needs test signing);
  3. Qualcomm QCS6490.WP BSP (NDA, not obtainable).
- **A Qualcomm-shaped DSDT:**
  - `PEP0` and its resource packages (~11k lines in the SC7280 DSDT);
  - `_SUB` board ID;
  - the `PRPx`/`SOID`/`QUFN` names (`prior-art.md` §1.2, `q6a-firmware.md` §3.4);
  - an IORT built from the `xbl_config` `/soc/iort` data (`q6a-fw/xbl_config/iort-summary.txt`);
  - CSRT (GPI DMA, HAL extensions).

**Work items:**
1. **"Bring-your-own AML" loader.** AcpiPlatformDxe replaces or adds DSDT/SSDT/IORT/CSRT from user-supplied `.aml` files on the ESP (e.g. `\EFI\RubikPi3\acpi\`). Qualcomm-derived tables then never enter the repo (design **INFERRED**).
2. Open-source IORT with our ACPI paths, and DSDT skeleton devices with `_STA` gated off by default.
3. Bring the drivers up one at a time:
   1. `qcpep` with a minimal PEP package;
   2. `qcgpio`, then `qci2c`/`qcspi`/`qcuart` (header);
   3. `qcsubsys`/`qcpil` (Windows DSP images, which differ from the Linux AudioReach images, **INFERRED**);
   4. `qcdx`.
4. **HDMI with qcdx.** qcdx owns GPU **and** display; installing it disables BasicDisplay. Options:
   - (a) a DSI0 1080p panel XML through `_ROM`, plus an LT9611 re-lock helper (KMDF on I2C9; reset PCR at 0x8011, `Q/Drivers/MdssDisplayDxe/Lt9611.c:40-45`), **INFERRED**;
   - (b) DP alt mode over USB-C (needs pmic_glink through the ADSP);
   - (c) a USB display adapter with an ARM64 driver (**INFERRED**);
   - (d) a full LT9611 Windows driver: unlikely.
5. **Audio:** a custom LPASS MI2S + ES8316 driver modelled on CoolStar's `csaudiork3x`/`es8323` (local `RkDrvPkg_ARM64_Release_v0.2_testsigned`), or USB audio.

**Verification:** each driver loads without a code 10/28/31 error; there is no 0x14E; DVFS is visible
(Task Manager speed changes); GPU is visible in `dxdiag`. HDMI must stay usable at each step.

- **Effort:** months, uncertain. A rough guess (**INFERRED**):
  - PEP and buses: 20-40 ED;
  - DSPs: 15-30 ED;
  - qcdx with LT9611: 20-60 ED, possibly infeasible.

**Risks:**
- legal (§8);
- driver HW-ID/SoC-ID checks (**INFERRED**);
- the PEP bugcheck;
- test-signing requires Secure Boot off;
- WU drivers target CHIDs of Qualcomm reference designs.

---------------------------------------------------------------------------------------------------

## 5. Device scope matrix

| Device | Windows path | Phase | Likelihood |
|---|---|---|---|
| CPU (8 cores), GIC, timers, PSCI | MADT/GTDT/FADT, inbox | 1 | High |
| HDMI (DSI0 → LT9611) | BasicDisplay on the kept GOP framebuffer, 1080p60 fixed, no hot-plug, no audio | 1 | High (after firmware work) |
| Debug UART (kd) | DBG2 GENI 0x11/0x13, inbox kdcom | 1 | High (**INFERRED** clock choice) |
| UFS 128 GB | inbox storufs `QCOM24A5` | 2 | High |
| USB 2.0 Type-A (usb_2) | inbox usbxhci `PNP0D10` | 2 | High |
| USB 3.0 Type-A x2 (Renesas, PCIe0) | inbox pci + usbxhci | 3 | Med |
| Gigabit Ethernet (AX88179B, behind the Renesas) | inbox `netax88179_178a` / `usbncm` | 3 | Med (High once PCIe0 works) |
| RTC, RNG, SMBIOS, variables, Secure Boot | firmware | 4 | High / Med (persistence) |
| Fan, LED | fixed in firmware | 4 | High |
| CPU DVFS, thermal management | PEP (Tier B) or `_CPC`/thermal zones (**INFERRED**) | 4/5/6 | Low-Med |
| M.2 NVMe (PCIe1) | inbox stornvme | 5 | Med-High |
| USB-C (usb_1) | forced host via `PNP0D10`, or URS (Tier B) | 5/6 | Low-Med |
| Wi-Fi (AP6256 SDIO) | none | - | **None** (USB or M.2 instead) |
| Bluetooth (BCM4345C5 UART) | WoR driver + a UART driver | 5 | Low |
| 40-pin GPIO/I2C/SPI/UART | qcgpio/qci2c/qcspi/qcuart + rhproxy (Tier B) | 6 | Med (with drivers) |
| GPU / video (Adreno 643, Venus) | qcdx (Tier B) | 6 | Low-Med |
| ADSP/CDSP | qcsubsys/qcpil at EL1 (Tier B) | 6 | Med with the Radxa pack |
| Headphone jack (ES8316), HDMI audio | custom driver | 6 | Low (use USB audio) |
| Cameras (IMX219/477/708) | Qualcomm camera stack, no RPi sensor support | - | Low (use a UVC webcam) |
| TPM | none (Qualcomm fTPM TAs absent) | - | None: bypass / IoT edition |

---------------------------------------------------------------------------------------------------

## 6. Blockers and hard limits

1. **No GPU acceleration on HDMI.** HDMI is DSI → plain LT9611, which the host programs over I2C (`Lt9611.c:1-22`). Qualcomm's display driver handles bridges only as fixed "panels", and its UEFI profile is for the LT9611**UXC** (`board-peripherals.md` §4.1). No public Windows LT9611 component exists. Thundercomm's promised open-source "LT9611 MDPPlatformLib" is for UEFI and its status is unknown. Realistic result: BasicDisplay at 1080p60 only, and qcdx would take the display away.
2. **On-board Wi-Fi: none.** The BCM43456 over SDIO has no Windows ARM64 driver; the WCN6750/WPSS path is not populated (`M:23-35`).
3. **PCIe is not ECAM.** It is solvable (iATU shift mode / NXPMX6), but bus-0 aliasing, UR handling and INTx are all untested on this board. There is no ITS, so MSI needs the MBI frame. Ethernet and the USB 3 ports depend on this.
4. **No PEP, so no power management.** There is no DVFS, no thermal control and no sleep (S0ix); the fan runs at a fixed duty. Idle states (`_LPI`) must not be exposed, because the UEFI's RPMh votes are active-only (`Q/Library/Qcs6490RpmhLib/Rpmh.c:1-12`, **INFERRED**).
5. **DSPs only at EL1** (TrustZone policy, README). Any Qualcomm DSP-backed feature therefore forces EL1, together with its SMMU and >4 GiB DMA caveats.
6. **Runtime variable persistence.** Windows' runtime writes stay in RAM until Phase 4.7. Secure Boot servicing (dbx) depends on that work.
7. **No TPM.** BitLocker needs a password or USB key; Windows Hello has no TPM-backed keys; consumer Setup needs bypasses; feature updates on bypassed installs may need the bypass again (**INFERRED**).
8. **UFS LUN exposure.** Windows sees the firmware LUNs and cannot be told to hide them through ACPI. The mitigation is documentation (or using NVMe, Phase 5.1).
9. **Microsoft Secure Boot 2011 CAs expire** (2026-06/10, `win-requirements.md` §2). Ship the 2023 certificates when Secure Boot is added.

---------------------------------------------------------------------------------------------------

## 7. Decisions only the user can make

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Edition and media | (a) consumer 25H2 ISO (local) with LabConfig bypasses; (b) Windows 11 IoT Enterprise LTSC 2024 (90-day eval; officially lists QCS6490; TPM and Secure Boot optional; Arm64 Hyper-V) | (a) for bring-up, (b) for anything long-lived. A licence is needed to activate either |
| D2 | EL for Windows | (a) EL2: early SEC Gunyah exit, DSP preload off; SMMU bypass, Hyper-V possible, no DSPs; no Qualcomm prior art. (b) EL1 under Gunyah: Qualcomm's design, needed for Tier B, SMMU keep-list, >4 GiB DMA risk, no Hyper-V | (a) for Tier A, with (b) kept working as the fallback and for Tier B |
| D3 | How a Windows boot selects its EL in dual-boot | (a) manual settings (Hypervisor = EL2 + DSP Preload = Disabled); (b) a new "OS profile" setting that sets both; (c) automatic: keep the late-exit default and have Windows fall back to EL1 | (b), with (c) as the safety net built in Phase 1 |
| D4 | DT/ACPI default | ACPI+DT (Linux keeps DT, Windows uses ACPI) / ACPI only / DT only | ACPI+DT with the setting; DT-only stays available |
| D5 | TPM / Secure Boot | bypass now; Secure Boot later with Microsoft keys supplied at build time (never committed); no TPM | as stated |
| D6 | Disk layout | (a) Windows and Linux share UFS LUN 0 (repartition the flange image); (b) Windows takes LUN 0, Linux moves to USB or NVMe; (c) Windows on NVMe after Phase 5.1 | (a) or (c); decide before Phase 2 |
| D7 | Qualcomm driver source (Tier B) | none; WOA-Project 7280 WU mirror (production-signed, laptop HIDs, redistribution unclear); Radxa Q6A pack (test-signed, QCS6490 HIDs); Qualcomm BSP (NDA) | Decide only after Tier A works. Whichever is chosen, the user downloads it and it stays out of git |
| D8 | ACPI table authorship | (a) our own ASL from the Linux DT and specs (repo-clean); (b) adapt the public SC7280 dumps or Aloha's AML (copyright, reference only); (c) extract the Radxa WP firmware tables (Qualcomm licence §1.3 forbids reverse engineering) | (a) in the repo. (b)/(c) only as user-supplied overlays through the Phase 6 loader, at the user's own legal risk |
| D9 | Device scope | Tier A only / Tier A + selected Tier B / everything | Tier A + PCIe1, then reassess |
| D10 | Variable persistence depth | RAM + BDS entry / + warm-reset carry / + runtime UFS writes | through 4.7a; 4.7b only if Secure Boot servicing matters |
| D11 | Fan duty under Windows | fixed percentage | a safe fixed value (e.g. 60-100 %), measured in 4.5 |
| D12 | Upstreaming | keep in the flange tree / also send ACPI work upstream (edk2-platforms) | own-ASL tables are upstreamable; overlays are not |

---------------------------------------------------------------------------------------------------

## 8. Legal and repository hygiene (applies to every phase)

- **Never in git:** Microsoft binaries (ISO content, bootmgr/winload, drivers, Secure Boot certificates) and Qualcomm binaries (drivers, firmware, DSP images, extracted tables). `workspace/` is git-ignored for analysis copies (`.gitignore:4`).
- **ACPI in the repo** is written from the Linux DT (GPL-2.0/BSD) and from the ACPI, Arm and Microsoft specs. The public Qualcomm dumps are used only to learn IDs and layout (`prior-art.md` §6); no PEP packages are copied.
- **Microsoft Secure Boot keys** are passed to the build from outside (`-D DEFAULT_KEYS=TRUE -D KEK_DEFAULT_FILE1=…`, RK pattern).
- **The Renesas `renesas_usb_fw.mem`** keeps being read from the board's own `usb_fw` partition at run time.
- **Radxa downloads** (WP firmware, Q6A driver pack) and the WOA-Project driver mirror are the user's decision (D7, D8). They are for personal use on the user's own board only.

---------------------------------------------------------------------------------------------------

## 9. Timeline sketch (single developer)

| Phase | ED | Cumulative | Milestone |
|---|---|---|---|
| 0 | 3-6 | ~1 week | experiments answered, media ready |
| 1 | 10-18 | ~4 weeks | WinPE Setup on HDMI |
| 2 | 8-15 | ~7 weeks | Windows desktop from UFS (USB 2.0 port) |
| 3 | 6-15 | ~10 weeks | USB 3 and Ethernet |
| 4 | 30-60 | ~4-6 months | daily-usable Tier A: time, Secure Boot, persistence, fan |
| 5 | 20-60 | +1-3 months | NVMe, USB-C, Wi-Fi replacement |
| 6 | 60-130+ | open | Qualcomm stack, decision-gated |

## 10. Sources

- Reports in this directory, as cited per item: `win-requirements.md`, `prior-art.md`, `q6a-firmware.md`, `firmware-gaps.md`, `board-peripherals.md`, `bootmgr-winload.md` (partial). Evidence directories: `dis/`, `win-reqs/`, `fwgap/`, `q6a-fw/`, `prior-art-acpi/`.
- **Spot-checked source lines:**
  - `Q/Drivers/MdssDisplayDxe/MdssDisplayDxe.c:141-160,351-358`
  - `Q/Drivers/SmmuDxe/SmmuDxe.c:1-29,100-105,255-269,714-724`
  - `Q/Drivers/NvStoreFvbDxe/NvStoreFvbDxe.c:1656-1660,1716-1718,1773-1810`
  - `Q/Drivers/GunyahExitDxe/GunyahExitDxe.c:269-289,356-361`
  - `Q/QCS6490.dsc.inc:106,136,151,157-158,177,516,641`
  - `Q/QCS6490.fdf:66-247`
  - `RK/Drivers/ExitBootServicesHookDxe/OsIdentification.c:41`
  - `L/drivers/ufs/host/ufs-qcom.c:2439` (`QCOM24A5`) and `L/drivers/usb/host/xhci-plat.c:624-625` (`PNP0D10`/`PNP0D15`), which make the `acpi=force` smoke test possible
  - `edk2-qualcomm/README.md` ("Exception level", "DSPs at EL2", "USB", "Settings")
- **External** (see the reports for full lists):
  - Microsoft bring-up docs (ACPI tables, DBG2, UEFI requirements, BasicDisplay)
  - Radxa Q6A Windows docs and forum threads 29913 and 28886
  - Mu-Silicium and Project Aloha
  - WOA-Project Qualcomm-Reference-Drivers
  - edk2-rk3588
  - TravMurav Qcom-Secure-Launch
