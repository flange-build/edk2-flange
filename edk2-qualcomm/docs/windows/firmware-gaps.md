# Windows 11 ARM64 on the RUBIK Pi 3: firmware gap analysis and implementation designs

Scope: our firmware (`edk2-qualcomm/`, QCS6490 = SC7280 "kodiak"), compared with what Windows 11 25H2 ARM64 needs to
install from USB and run from UFS. The report gives a concrete design for each gap. It cites file:line for our code
(`Q/` = `edk2-qualcomm/Silicon/Qualcomm/QCS6490/`, `P/` = `edk2-qualcomm/Platform/Thundercomm/RubikPi3/`), for
edk2-rockchip (`RK/` = `edk2-rockchip/Silicon/Rockchip/`), for Linux 7.0.2 (`L/`) and for local artefacts. Claims marked
**INFERRED** are not verified on hardware or in the vendor source.

Related sibling reports in this directory: `prior-art.md` (public SC7280 Windows ACPI dumps, Radxa Q6A) and
`q6a-firmware.md` (the Q6A flat build has no ACPI). Section 5.1 of `prior-art.md` supplies the Qualcomm SC7280
Windows-table facts used below. Those dumps are reference material only. All ASL proposed here is written from the
Linux DT and the specs.

Working copies made for this report: `fwgap/2/Windows/INF/*.inf`, inbox INFs extracted from `sources/boot.wim`
image 2 (WinPE/Setup), and `fwgap/bootwim.list`, the full WIM listing.

---------------------------------------------------------------------------------------------------

## 0. Summary of findings and decisions

| # | Area | Today | Gap | Decision |
|---|---|---|---|---|
| a | ACPI | No ACPI module or table in the FDF (`Q/QCS6490.fdf:87-247`). DT only, via DtPlatformDxe (`:165`). | **Blocking.** bootmgr needs RSDP → XSDT → FADT. | Static ASL/aslc tables plus a small `Qcs6490AcpiPlatformDxe` (RK model). Expose ACPI and DT together by default; Linux keeps using DT. |
| b | Display after EBS | MdssDisplayDxe tears HDMI down in BeforeExitBootServices (`Q/Drivers/MdssDisplayDxe/MdssDisplayDxe.c:141-160,351-358`). SmmuDxe removes the MDSS stream at EBS. | **Blocking for a usable install.** No picture once winload exits boot services. | "Windows handoff" mode: skip `DisplayStop()`, keep the MDSS SMMU entry, keep the votes. Framebuffer is already `EfiReservedMemoryType` below 4 GiB (`MdssDisplayDxe.c:189-195`). |
| c | Variables at runtime | The FVB refuses runtime writes with `EFI_WRITE_PROTECTED` (`Q/Drivers/NvStoreFvbDxe/NvStoreFvbDxe.c:1656-1660,1716-1718`). The RT properties table says no SetVariable (`:1773-1810`). | **High risk.** Setup's bcdboot SetVariable fails (**INFERRED**: Setup may abort). | Phase 1: accept runtime writes into RAM for Windows boots, and have BDS create a "Windows Boot Manager" entry itself. Phase 2: carry dirty blocks across a warm reset. Phase 3: runtime UFS flush in ResetSystem. |
| d | PCIe | Non-ECAM PciSegmentLib: DBI for 00:00.0, a 4 KiB CFG0 window retargeted per access (`Q/Library/Qcs6490PciSegmentLib/PciSegmentLib.c:1-20,92-140`). | Windows needs ECAM via MCFG. | The address map is already ECAM-shaped: 0x60000000 = bus 0 (DBI), 0x60100000 = bus 1. Program iATU region 0 once in **CFG-shift mode**. MCFG seg 0 = 0x60000000, buses 0–1 (Qualcomm's own SC7280 MCFG does exactly this). Use INTx `_PRT` at GSIV 181–184. |
| e | USB | usb_2 is a NonDiscoverable xHCI in UEFI. Renesas firmware is loaded. No ACPI. | Windows needs ACPI xHCI nodes, SMMU bypass that survives EBS, and PCIe (d). | `XHC0` `_HID QCOM0AA1`/`_CID PNP0D15`, 0x08C00000, GSIV 274. Renesas via `PCI0`. usb_1 (Type-C) deferred: pmic_glink/ADSP dependent. |
| f | UFS | UFS works in UEFI. SmmuDxe drops the UFS bypass at EBS (`Q/Drivers/SmmuDxe/SmmuDxe.c:255-269,714-724`). | Boot disk unusable in Windows at EL1 unless the entry is kept. | `UFS0` `_HID QCOM24A5` (inbox storufs, `fwgap/.../storufs.inf:84`), 0x01D84000, GSIV 297. Keep SID 0x80 at EL1, or run at EL2. |
| g | EL/SMMU | EL1 (Gunyah) or EL2 is decided in SEC. Late exit happens at EBS (GunyahExitDxe). | Windows has no SMMU driver. Late exit is unsafe for winload. | **Recommended: EL2 via an early (SEC) Gunyah exit, DSP preload off.** All DMA bypasses the SMMU and >4 GiB DMA is not at risk. Fallback: EL1 with the SmmuDxe "Windows keeps bypass streams" mode. Never do the late EBS exit under winload. |
| h | TPM/SB/RTC/WDT | TPM/Auth libs are Null (`Q/QCS6490.dsc.inc:157-158`). The RTC is virtual (`:136`). The watchdog belongs to Gunyah. | Win11 Setup checks TPM 2.0 and Secure Boot. Time does not survive a reboot. | Bypass the checks (LabConfig). Optional Secure Boot via AuthVariableLib with external keys. PMK8350 RTC runtime lib over SPMI, disabled for Linux (RK RuntimeServicesManagerDxe pattern). No TPM path (no TrEE fTPM TA). No WDAT initially. |

Cross-cutting new piece: a **single ExitBootServices wrapper with OS identification**. It merges RK's
ExitBootServicesHookDxe with our GunyahExitDxe and must recognise both `winload` **and** `winresume` (Fast Startup and
hibernation resume). Sections b, c, d, f and g use it.

---------------------------------------------------------------------------------------------------

## 1. Baseline facts about our firmware (what Windows will see today)

* **FV contents** (`Q/QCS6490.fdf:87-247`):
  * no `AcpiTableDxe`, no ACPI tables, no BGRT;
  * DT installed by `EmbeddedPkg/Drivers/DtPlatformDxe` (`:165`) from `P/DeviceTree/Mainline.inf` (`P/RubikPi3.Modules.fdf.inc:8`);
  * SMBIOS present (`:185-188`), entry point version 0x0307 (`Q/QCS6490.dsc.inc:476`), so an SMBIOS 3 (64-bit) entry point is produced;
  * the shell links the `acpiview` command lib (`Q/QCS6490.dsc.inc:516`), handy for checking our tables later;
  * the FDF already includes `ArmVirtPkg/ArmVirtRules.fdf.inc` (`:281`), which defines `[Rule.Common.USER_DEFINED.ACPITABLE]` (`edk2/ArmVirtPkg/ArmVirtRules.fdf.inc:110-115`).
* **Size budget.** FVMAIN_COMPACT uses 0x13F7F0 bytes (RELEASE) and 0x190DA8 bytes (DEBUG) of 0x300000
  (`workspace/Build/RubikPi3/*/FV/FVMAIN_COMPACT.Fv.txt`). About 1.4 MiB of compressed space is left for ACPI,
  Secure Boot and an RTC driver. The uefi_a partition is 5 MiB (`configs/rubikpi3: UEFI_PARTITION_SIZE=0x500000`).
* **Memory map** (`Q/Library/Qcs6490Lib/Qcs6490Mem.c`):
  * carve-outs are `EfiReservedMemoryType` and unmapped (`:54-71`, `:253-255`);
  * the variable store at 0xA0000000 is `EfiRuntimeServicesData` (`:355`);
  * peripherals 0–0x80000000 are mapped as device memory (`:482-486`);
  * DRAM comes from SMEM: 8 GiB, so physical addresses above 4 GiB exist and Windows will use them;
  * runtime drivers are linked at 64 KiB alignment (`Q/QCS6490.dsc.inc:264`), as Windows ARM64 needs.
* **Runtime services.**
  * Reset is PSCI (`Q/QCS6490.dsc.inc:106`).
  * The RTC is `VirtualRealTimeClockLib` (`:136`): no hardware, and time does not survive power loss.
  * Variables live in RAM and are flushed only at boot time (section 4).
  * Capsules are Null (`:175`), so no ESRT and Windows Update will not push firmware.
  * RNG is `BaseRngLibTimerLib` (`:151`) with no `EFI_RNG_PROTOCOL` driver. bootmgr/winload then use weaker entropy (**INFERRED**: tolerated).
* **Secure Boot / TPM.**
  * `TpmMeasurementLibNull` and `AuthVariableLibNull` (`Q/QCS6490.dsc.inc:157-158`).
  * `DxeImageVerificationLib` is linked into SecurityStubDxe (`:577-580`), but without authenticated variables there can be no db/dbx, so Secure Boot is effectively off.
* **BDS.** `ArmPkg/Library/PlatformBootManagerLib` (`Q/QCS6490.dsc.inc:177`). Boot options for every device are refreshed
  when the discovery policy changes (`edk2/ArmPkg/Library/PlatformBootManagerLib/PlatformBm.c:940-975`) and in
  `PlatformBootManagerUnableToBoot` (`:1111-1134`). A UFS LUN option boots `\EFI\BOOT\BOOTAA64.EFI` from that LUN's ESP.
* **ExitBootServices handlers that undo state today:**
  1. MdssDisplayDxe `DisplayStop()` in the BeforeExitBootServices group (`MdssDisplayDxe.c:141-160`);
  2. NvStoreFvbDxe's last write-back, after which it stops touching UFS (`NvStoreFvbDxe.c:1015-1040`);
  3. SmmuDxe `RestoreSmmu` (EVT_SIGNAL_EXIT_BOOT_SERVICES at TPL_NOTIFY, `SmmuDxe.c:255-269,714-724`), which removes the UFS, MDSS, USB2 and PCIE0 bypass entries (`:100-105`);
  4. XhciDxe halts both xHCs (comment at `SmmuDxe.c:22-29`);
  5. GunyahExitDxe wraps `gBS->ExitBootServices` when the exit is deferred (`Q/Drivers/GunyahExitDxe/GunyahExitDxe.c:269-289,356-361`).

---------------------------------------------------------------------------------------------------

## 2. (a) ACPI infrastructure

### 2.1 How edk2-rockchip provides ACPI for Windows on RK3588

* **Static tables, built as one FFS file with the well-known GUID.**
  * Silicon tables live in `RK/RK3588/AcpiTables/`: `Fadt.aslc`, `Madt.aslc`, `Gtdt.aslc`, `Mcfg.aslc`, `Spcr.aslc`, `Dbg2.aslc`, `Pptt.aslc`, `DsdtCommon.asl`, `Pcie.asl`, `Usb*.asl`, …
  * The board part is `Platform/<board>/AcpiTables/{AcpiTables.inf,Dsdt.asl}`, e.g. `Platform/Radxa/ROCK5B/AcpiTables/AcpiTables.inf`: FILE_GUID `7E374E25-8E01-4FEE-87F2-390C23C606CD`, sources at `:26-33`.
  * It is included with `INF RuleOverride = ACPITABLE …` (`Platform/Radxa/ROCK5B/ROCK5B.Modules.fdf.inc:10`). The board's `Dsdt.asl` `#include`s the silicon `.asl` files.
* **Modules.**
  * `MdeModulePkg/Universal/Acpi/AcpiTableDxe` with `PcdInstallAcpiSdtProtocol=TRUE` (`RK/Rockchip.dsc.inc:382,605-609`).
  * `BootGraphicsResourceTableDxe`.
  * `RK/RK3588/Drivers/AcpiPlatformDxe` (`AcpiLib|EmbeddedPkg/Library/AcpiLib`, `RK/RK3588/RK3588Base.dsc.inc:131,447`).
* **AcpiPlatformDxe** (`RK/RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c`):
  * **Gating:** it returns `EFI_UNSUPPORTED` unless `PcdConfigTableMode & CONFIG_TABLE_MODE_ACPI` (`:573-576`).
  * **EndOfDxe:** `LocateAndInstallAcpiFromFvConditional(&mAcpiTableFile)` installs every table of the FFS file (`:150-205`, call at `:173`). It then opens the DSDT through `EFI_ACPI_SDT_PROTOCOL` and patches `_STA` of absent controllers to 0 with `AcpiAmlObjectUpdateInteger` (`:103-148`).
  * **ExitBootServices, per OS:** a handler registered with `gExitBootServicesOsNotifyProtocolGuid` (`:394-427,598-610`) patches by OS type. For Windows it hides the EHCI `_CID` through `EHID=0` (`:412-414`). It marks the SD slot non-removable when booted from SD (`:420-422`). It rewrites MCFG/DSDT for the chosen ECAM mode (`:207-392`), then fixes checksums.
  * **NameOp patcher** `AcpiUpdateSdtNameInteger()` (`:43-101`) is safe at EBS: it allocates nothing.
* **OS identification** (`RK/Drivers/ExitBootServicesHookDxe/`):
  * `gBS->ExitBootServices` is wrapped (`ExitBootServicesHookDxe.c:137-163`).
  * The caller's return address is walked back to its PE header (`OsIdentification.c:72-94`) and classified:
    * Linux by the arm64 Image magic at +0x38 (`:28-39`);
    * Windows if the CodeView PDB path contains `winload` (`:41-66`).
  * The handlers run before the original EBS, so before the BeforeExitBootServices group. The hook then **restores the original pointer** and calls it (`ExitBootServicesHookDxe.c:131-134`; see 2.2.3 for why we must not copy that).
* **ConfigTableMode** (`RK/RK3588/RK3588Base.dsc.inc:50-52,283,373`; HII at `RK3588DxeHii.vfr:805-812`): ACPI = 1, FDT = 2, ACPI+FDT = 3 (the default). FdtPlatformDxe is likewise gated (`RK/RK3588/Drivers/FdtPlatformDxe/FdtPlatformDxe.c:1418-1421`).
* **How Linux keeps using DT.** arm64 Linux enables ACPI only if `acpi=on|force` or the DT is a stub (only `/chosen`)
  (`L/arch/arm64/kernel/acpi.c:198-209`). With both tables installed, Linux uses DT and Windows uses ACPI (Windows ignores
  the FDT configuration table).
* **Runtime-service arbitration.** `RK/Drivers/RuntimeServicesManagerDxe/RuntimeServicesManagerDxe.c:62-90` replaces
  `gRT->GetTime/SetTime/...` with `EFI_UNSUPPORTED` stubs when a Linux kernel with an FDT is booted. Linux then uses its own RTC
  driver and does not race the firmware on the bus.
* **Windows quirks RK learned:**
  * EHCI must be hidden: the inbox driver uses atomics on uncached memory (`AcpiPlatformDxe.c:408-414`).
  * The GIC MSI frame is disabled "as some OSes are not happy about it (like Windows)" (`RK/RK3588/AcpiTables/Madt.aslc:180-194`). This is RK-specific: Qualcomm's own SC7280 MADT has one, see 2.2.4.
  * No ITS/IORT for Windows (`AcpiPlatformDxe.c:266-268`, `Madt.aslc:196`). PCIe therefore uses legacy INTx through `_PRT` (`RK/RK3588/AcpiTables/Pcie.asl:27-32`).
  * FADT `IaPcBootArch = MSI_NOT_SUPPORTED` (`RK/RK3588/AcpiTables/Fadt.aslc:61`).
  * `_CCA 0` everywhere (`Pcie.asl:15`, `Usb3Host0.asl`).

### 2.2 Design for QCS6490

#### 2.2.1 Files and modules

```
Silicon/Qualcomm/QCS6490/AcpiTables/            (new, silicon)
  AcpiTables.h      OEM ID/table ID macros, GSIV constants, MCFG struct
  Fadt.aslc  Madt.aslc  Gtdt.aslc  Mcfg.aslc  Spcr.aslc  Dbg2.aslc  Pptt.aslc
  Cpu.asl  Ufs.asl  Usb2.asl  Pcie0.asl  Common.asl (\_SB._OSC, RES0 for GIC/etc.)
Platform/Thundercomm/RubikPi3/AcpiTables/       (new, board)
  AcpiTables.inf    FILE_GUID 7E374E25-8E01-4FEE-87F2-390C23C606CD, MODULE_TYPE USER_DEFINED
  Dsdt.asl          DefinitionBlock + #include of the silicon .asl (board picks what exists)
Silicon/Qualcomm/QCS6490/Drivers/AcpiPlatformDxe/  (new) Qcs6490AcpiPlatformDxe
Silicon/Qualcomm/QCS6490/Drivers/HandoffDxe/       (new, or GunyahExitDxe extended) EBS wrapper + OS type
```

DSC changes (`Q/QCS6490.dsc.inc`):
* `AcpiLib|EmbeddedPkg/Library/AcpiLib/AcpiLib.inf`;
* `gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE`;
* components `MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf`, `.../BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf`, `Silicon/.../AcpiPlatformDxe`, `.../HandoffDxe`;
* the board DSC lists `$(PLATFORM_DIRECTORY)/AcpiTables/AcpiTables.inf`.

FDF changes (`Q/QCS6490.fdf` FvMain): the three INFs. The board FDF include adds
`INF RuleOverride = ACPITABLE $(PLATFORM_DIRECTORY)/AcpiTables/AcpiTables.inf`.

#### 2.2.2 Qcs6490AcpiPlatformDxe behaviour

1. **Gating.** It loads only if the new `ConfigTableMode` setting has the ACPI bit. Add it to PlatformConfigDxe as
   variable `ConfigTableMode`, vendor GUID `gQcs6490PlatformConfigGuid`, values ACPI = 1 / DT = 2 / ACPI+DT = 3, default 3.
   DtPlatformDxe needs the matching DT-bit gate: wrap it or replace the stock `DtPlatformDxe` with a gated copy.
2. **EndOfDxe:**
   * `LocateAndInstallAcpiFromFvConditional`, as RK.
   * Patch `\_SB.PCI0._STA` to 0 when PCIe0 did not come up. With the link down, a config access to bus 1 can SError (`Q/Include/Qcs6490Pcie.h:133-153`), so Windows must not see `PCI0`. PCIe0 is trained inside `PciHostBridgeGetRootBridges()` during DXE dispatch (`Q/Library/Qcs6490PciHostBridgeLib/PciHostBridgeLib.c:154-185`), so the result is known by EndOfDxe. Publish it through a new dynamic PCD `PcdPcie0LinkUp` or a tiny protocol.
   * Patch `\_SB.XHC0._STA` when `Dwc3HostDxe` failed, and `\_SB.UFS0._STA` likewise.
3. **ExitBootServices (OS = Windows):**
   * optional PCIe quirk mode (2.2.5 / section 5);
   * flip RT-properties bits for Windows (section 4);
   * checksum fix-ups.

   No patching is needed for Linux, which ignores ACPI.

#### 2.2.3 One ExitBootServices wrapper ("HandoffDxe")

We already wrap EBS in GunyahExitDxe. RK's hook writes the original pointer back before calling it
(`RK/Drivers/ExitBootServicesHookDxe/ExitBootServicesHookDxe.c:134`), which would unhook a second wrapper. A second
wrapper installed later also breaks `RETURN_ADDRESS(0)` OS identification, because the caller would be the other
wrapper, not winload. So merge both into one wrapper that is always loaded:

1. `OsType = Identify(FindPeImageBase(RETURN_ADDRESS(0)))`. Copy `OsIdentification.c`, extended:
   * Windows if the PDB path contains `winload` **or `winresume`**. winresume is the loader used on every boot after a
     Fast Startup shutdown and on hibernation resume; RK's code does not match it (`OsIdentification.c:41`).
   * Linux by the Image magic. GRUB also calls EBS through the kernel's stub, so magic detection works there.
2. Run the registered handlers (protocol `QCS6490_HANDOFF_PROTOCOL { GetOsType(); RegisterHandler(); }`). They run
   before the original EBS, so before BeforeExitBootServices. MdssDisplayDxe, SmmuDxe, NvStoreFvbDxe, AcpiPlatformDxe and
   the RTC lib read `GetOsType()` from their own events.
3. Call the original EBS. Retry semantics as today (`GunyahExitDxe.c:269-289`).
4. Deferred Gunyah exit:
   * OS != Windows: as today.
   * OS == Windows: **do not exit** (stay at EL1, see section 8), and keep the SMMU entries.

   Rationale: winload began running at EL1. Switching it to EL2 in the middle of the handoff is untested. Winload sets up EL1
   system registers for the kernel after EBS; at EL2 (non-VHE) those writes would not affect the running regime
   (**INFERRED**, high risk). Linux's stub is safe because the kernel checks CurrentEL at entry.

#### 2.2.4 Table set (concrete values; all from kodiak.dtsi / board dumps)

Interrupt numbering: GSIV = SPI + 32, PPI + 16.

* **FADT** (rev 6.3+, as `RK/RK3588/AcpiTables/Fadt.aslc`):
  * `HW_REDUCED_ACPI`; ArmBootArch `PSCI_COMPLIANT`, no `PSCI_USE_HVC`. The PSCI conduit is SMC: dmesg "SMC Calling Convention v1.1" (`workspace/usb-research/board/dmesg_full.txt`).
  * `IaPcBootArch = MSI_NOT_SUPPORTED` while we use INTx; drop it if the MBI frame is enabled.
  * PM profile Desktop/Appliance. No `LOW_POWER_S0_IDLE` (no PEP, no Modern Standby).
  * Optional "Hypervisor Vendor Identity" = "QCOM" only when at EL1. Qualcomm sets it (`prior-art.md` §5.1).
* **MADT:**
  * GICD 0x17A00000, GICv3 (`kodiak.dtsi:6877-6881`).
  * GICR range 0x17A60000, length 0x100000, 8 frames of 0x20000 (dmesg: CPU1 redistributor at 0x17a80000, `dmesg_full.txt:137`).
  * 8 GICC entries, UIDs 0–7, MPIDR 0x000…0x700 (`kodiak.dtsi:204-379`).
  * PMU GSIV 23 (PPI 7, `kodiak.dtsi:856-864`); VGIC maintenance GSIV 25 (PPI 9, `:6881`).
  * EfficiencyClass 0 for CPUs 0–3 (A55) and 1 for CPUs 4–7 (A78), as in Qualcomm's SC7280 MADT (`prior-art-acpi/.../APIC.dsl`).
  * Optional GIC MSI frame: base 0x17A10000, SPI base 832, count 128, flag "SPI count/base select". This matches the live DT
    `mbi-alias`/`mbi-ranges` (`usb-research/board/live.dts:6963-6964`), dmesg "MBI range [832:959]" (`dmesg_full.txt:111`) and
    Qualcomm's own MADT (`APIC.dsl` subtable 0x0D). Keep it behind a setting. Phase 1 uses INTx (RK experience: `Madt.aslc:180-194`).
  * No ITS: `kodiak.dtsi:6888-6894` has status disabled, and dmesg says "No ITS available".
* **GTDT:**
  * Secure EL1 GSIV 29, NS EL1 30, virtual 27, NS EL2 26, all level and active-low (`kodiak.dtsi:7882-7886`).
  * CntControlBase / CntReadBase 0xFFFFFFFFFFFFFFFF, as Qualcomm and RK do.
  * Optional platform timer block 0x17C20000 / frame 0x17C21000, GSIV 40 / 38 (`kodiak.dtsi:6905-6918`). Leave it out at EL1 (**INFERRED**: Gunyah may own it).
* **MCFG:** one allocation, segment 0, base 0x60000000, buses 0–1 (section 5). Qualcomm's SC7280 Windows MCFG has
  this exact entry (`prior-art-acpi/.../MCFG.dsl:25-32`).
* **SPCR / DBG2:**
  * GENI UART5 at 0x994000, GSIV 638 (SPI 606, `kodiak.dtsi:1458-1464`), 115200 baud.
  * Interface subtype 0x11 ("SDM845, 1.8432 MHz") or 0x13 ("SDM845, 7.372 MHz") per Microsoft's DBG2 spec. Qualcomm's SC7280 tables use 0x11. Which one matches the clock XBL sets for uart5 is **INFERRED**; verify by reading the SE clock / serial divider before choosing.
  * Optional, but it gives EMS/SAC and kernel debugging.
* **PPTT:** one DSU cluster with 8 cores. Private L1/L2 per core, shared L3. Optional; RK ships one (`RK/RK3588/AcpiTables/Pptt.aslc`).
* **BGRT:** from BootGraphicsResourceTableDxe and our LogoDxe (Windows keeps the logo during boot).
* **DSDT:**

| Device | `_HID` / `_CID` | Resources (from kodiak.dtsi) | Notes |
|---|---|---|---|
| `\_SB.CPU0..7` | `ACPI0007` | `_UID` 0–7 = MADT UIDs | |
| `\_SB.UFS0` | `QCOM24A5`, `_CLS {0x01,0x09,0x01}` | Memory32Fixed 0x01D84000 + 0x3000 (Qualcomm uses 0x1C000, which includes ICE at 0x01D88000); Interrupt Level/High **297** (SPI 265, `kodiak.dtsi:2471-2476`) | `_CCA 0` first (our bypass path is non-coherent in UEFI); Qualcomm uses `_CCA 1` behind its SMMU driver. Inbox storufs binds `ACPI\QCOM24A5` with Qualcomm flags (`fwgap/.../storufs.inf:84,358-360`) |
| `\_SB.XHC0` (usb_2) | `QCOM0AA1`, `_CID "PNP0D15"` | Memory32Fixed 0x08C00000 + 0x100000; Interrupt Level/High **274** (SPI 242, `kodiak.dtsi:4378`) | `_CCA 0`; `RHUB.PRT1` `_UPC` Type-A, `_PLD`. usbxhci binds PNP0D15 (no debug capability) (`fwgap/.../usbxhci.inf:39-40`) |
| `\_SB.PCI0` | `PNP0A08` / `PNP0A03` | section 5 | `_SEG 0`, `_BBN 0`, `_CCA 0`, `_PRT` GSIV 181–184, `_OSC`, `RES0` PNP0C02 |
| `\_SB._OSC` | – | – | platform-wide `_OSC`, granting nothing special |

No `_DEP` on PEP0. We have no PEP; a `_DEP` on a missing PEP would leave devices stopped.

* **SMBIOS 3:**
  * Present: SMBIOS 3.x entry point (`Q/QCS6490.dsc.inc:476`), types 0/1/2/3/4/7/16/17/19 (`Q/Drivers/SmbiosMemoryDxe`, `Q/Library/OemMiscLib`).
  * Gap: serials and UUID are placeholders (`P/RubikPi3.dsc:95-108`, "SN0000"). Windows uses Type 1 UUID/serial for hardware IDs and activation. Derive them from the SoC serial (SMEM socinfo) **INFERRED**.

---------------------------------------------------------------------------------------------------

## 3. (b) "Windows mode" display: keep HDMI scanning out after ExitBootServices

### Today
* `MdssDisplayDxeInitialize` registers `OnBeforeExitBootServices` (`MdssDisplayDxe.c:351-358`). That calls
  `DisplayStop()` (`:107-138`): `DpuStop`, `Lt9611Disable`, `DsiHostDisable`, `DsiPhyDisable`, `DisplayPowerOff`, `Lt9611ReleaseBus`.
  * `DisplayPowerOff` also takes back the RPMh votes and leaves the RSC TCS clean (`Power.c:1061-1066`).
  * The rail votes stay, because the UFS/USB/PCIe PHYs share them (`Power.c:847-848`).
* SmmuDxe's `RestoreSmmu` removes the MDSS bypass entry (SID 0x900, mask 0x402, `SmmuDxe.c:102`) at EBS (`:714-724`).
* The framebuffer is 1920×1080×4 bytes, allocated `AllocateMaxAddress` below 4 GiB as `EfiReservedMemoryType`, mapped WC
  (`MdssDisplayDxe.c:177-219`). The comment at `:166-168` already foresees "left running for the OS".

### Design
* **Policy.** Setting "Display at OS handoff": Auto / Keep / Off.
  * Auto = Keep when HandoffDxe says Windows (winload/winresume), Off otherwise.
  * Linux and other OSes keep today's behaviour: the vendor and mainline kernels expect the display off (`MdssDisplayDxe.c:9-11`).
* **In `OnBeforeExitBootServices`:** if Keep, return without `DisplayStop()`. Leave `mStage == DisplayStageRunning` and the
  LT9611 pins as they are. Nothing in Windows will drive i2c9 (GPIO36/37) unless Qualcomm I2C drivers are added later; returning
  them to function 1 ("qup11") is harmless either way.
* **SMMU (EL1 only).** SmmuDxe must not restore the MDSS entry for Windows; see section 8. At EL2 the SMMU passes unmatched
  streams after the Gunyah exit, so nothing is needed.
* **What must stay on.** All of it is already on and nothing in Windows turns it off (Windows has no Qualcomm clock/RPMh/PEP driver):
  * DSI rails L6B 1.2 V and L10C 0.88 V in HPM (`Power.c:44-50`);
  * MMNOC bandwidth on BCM MM1/MM0 (`Power.c:643-720`);
  * the MDSS GDSC under software control (it is deliberately not handed to HW_CTRL, `Power.c:19-23`);
  * DISPCC MDP/AHB/byte/pixel/esc and the GCC_DISP_AHB/XO clocks;
  * DSI0 PHY PLL and DSI video engine;
  * DPU CTL0 / SSPP DMA0 / LM0 / INTF1 fetching the framebuffer;
  * LT9611 power (GPIO83) and reset (GPIO21).
* **Votes and idle states.** RPMh votes are sent as active-only from DRV2 of the apps RSC. They persist while the APSS stays
  active. Windows idles with WFI only unless we publish `_LPI` (we will not), so the RSC never enters its sleep/wake sets
  (**INFERRED**). Do not add `_LPI`/PSCI deep-idle states while the display depends on firmware votes.
* **Interrupts.** Mask the MDSS/DPU interrupt sources in hardware before handoff (MDSS `HW_INTR_EN` = 0), so nothing asserts a
  line Windows does not own. Windows never enables an SPI no device claims, so this is belt and braces.
* **Memory type.** Keep `EfiReservedMemoryType`, which Windows never reuses. RK uses `EfiRuntimeServicesData` below 4 GiB for the
  same purpose (`RK/Drivers/LcdGraphicsOutputDxe/LcdGraphicsOutputDxe.c:862-866`); either works. Windows' BasicDisplay takes
  the framebuffer base, size and stride from the GOP mode winload records.
* **Single mode.** 1080p60 only (`MdssDisplayDxe.c:32-45`). BasicDisplay cannot change modes, so 1080p is final.
* **Hot-plug.** A monitor replugged while Windows runs may need the LT9611 re-armed. UEFI has no HPD handling either; accept
  as a known limitation.
* **Hibernate / Fast Startup.** winresume also needs the display (Keep applies through the `winresume` match).

---------------------------------------------------------------------------------------------------

## 4. (c) Runtime variable persistence

### Today
* The variable driver writes through the FVB at runtime.
  * `NvStoreFvbWrite` / `NvStoreFvbEraseBlocks` return `EFI_WRITE_PROTECTED` when `EfiAtRuntime()` (`NvStoreFvbDxe.c:1656-1660,1716-1718`), so `SetVariable` fails at runtime with an FVB error.
  * The variable driver does not reclaim at runtime anyway (`edk2/MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c:1300-1305,2123-2133`).
* UFS is never touched after the BeforeExitBootServices flush (`NvStoreFvbDxe.c:1015-1040`). Reset notifications are not
  called at runtime (`edk2/MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystem.c:264`).
* `EFI_RT_PROPERTIES_TABLE` omits SetVariable (`NvStoreFvbDxe.c:1773-1810`). Linux honours it; Windows is not known to read it (**INFERRED**).

### What Windows writes at runtime
* Setup in WinPE calls bcdboot. By default bcdboot "adds a firmware entry in the NVRAM to point to the Windows Boot Manager"
  (Microsoft BCDBoot docs). Only with `/s` does it skip the NVRAM entry and rely on `\EFI\BOOT\BOOT<arch>.EFI`.
* Windows also writes:
  * `BootNext` / `OsIndications` (restart to firmware);
  * dbx updates (Secure Boot servicing);
  * `Boot####` repairs.
* If SetVariable fails, Setup may stop with "Windows could not update the computer's boot configuration" (**INFERRED**, risk;
  aarch64-laptops issue #25 reports firmware that does not expose runtime variable writes).

### Options (ranked)

1. **Phase 1. RAM-accepting runtime writes, for Windows boots.**
   * NvStoreFvbDxe gets a "runtime RAM mode", switched on by the HandoffDxe handler when OS = Windows. Write/Erase at runtime then
     update `mNvStore` and set a runtime dirty bitmap instead of returning `EFI_WRITE_PROTECTED`. This mirrors RkFvbDxe on SD/eMMC:
     "SD/eMMC (persistent only at boot time)" (`RK/Drivers/RkFvbDxe/RkFvbDxe.c:3-4,770-777`), where runtime writes update only
     the shadow copy and succeed.
   * Setup then completes. The NVRAM entry is lost at reboot.
   * For Windows, also set the SetVariable bits in the RT properties table in the same handler. The table is runtime pool;
     patching it in place before EBS is fine.
2. **Phase 1. BDS creates the Windows entry itself.**
   * Fork `ArmPkg/Library/PlatformBootManagerLib` into `Q/Library/PlatformBootManagerLib`, or add a BDS hook. After
     `EfiBootManagerConnectAll`/`RefreshAllBootOption`, scan FAT ESPs on fixed media for `\EFI\Microsoft\Boot\bootmgfw.efi`.
     If no `Boot####` points at one, add "Windows Boot Manager" with that file path, at the front of BootOrder unless the user
     has reordered.
   * It is written at boot time, so it is persisted to UFS.
   * It also covers the fallback: Windows places a copy of bootmgr at `\EFI\BOOT\BOOTAA64.EFI` (rodsbooks; **INFERRED** for
     ARM64). The existing auto-created "UFS LUN0" option boots that file.
3. **Phase 2. Carry runtime writes across a warm reset (cheap).**
   * On a runtime write, keep the dirty bitmap and a CRC in the status page (`Q/Include/Qcs6490NvStore.h:121-141`, add fields).
   * Implement `EfiResetWarm` and `EfiResetCold` as PSCI SYSTEM_RESET2 (warm) in a `Qcs6490ResetSystemLib` wrapper, so DDR
     stays in self-refresh.
   * SEC: if the status page says "runtime-dirty", the CRC of 0xA0000000… matches, and the boot counter matches, then use the
     memory copy instead of reading UFS, and flush it once UFS is up (NvStoreFvbDxe already writes dirty blocks back).
   * **INFERRED:** depends on XBL preserving 0xA0000000 across a warm reset (it is in XBL's UEFI load window,
     0x9FB00000–0xA0A00000, README "Memory map"). Test with a pattern before relying on it. Shutdown or power loss still
     loses the writes.
4. **Phase 3. Runtime UFS flush inside ResetSystem.**
   * Extend the SEC UFS code (`Q/Library/Qcs6490Lib/EarlyUfs.c`, read-only today) with WRITE(10) and SYNCHRONIZE CACHE, built
     as a runtime library: UFS MMIO 0x01D84000 registered `EFI_MEMORY_RUNTIME`, UTRD/UCD/PRDT in `EfiRuntimeServicesData`
     below 4 GiB.
   * `Qcs6490ResetSystemLib` calls it before PSCI. At that point Windows has flushed and quiesced storage; the code re-enables
     HCE and the link if needed, then sends START STOP UNIT (active) before writing.
   * Needs the UFS SMMU stream to bypass at runtime (EL1: kept for Windows anyway; EL2: bypass).
   * Linux arm64 reboots through PSCI, not EFI ResetSystem, so this is Windows-only (**INFERRED**: Windows uses EFI ResetSystem).
5. **Research. uefisecapp (QSEE).**
   * The RUBIK Pi TZ answers qseecom ("found qseecom with version 0x1402000", `dmesg_full.txt`). Linux uses uefisecapp for
     variables on the Dragon Q6A (`L/drivers/firmware/qcom/qcom_scm.c:2343`, allowlist), and the Q6A ships `uefi_sec.mbn`.
   * Storage behind uefisecapp on a UFS board goes through RPMB/GPT listeners, which need a non-secure-world service at runtime
     (Qualcomm Windows drivers). Not a near-term path.

---------------------------------------------------------------------------------------------------

## 5. (d) PCIe for Windows (ECAM via MCFG)

### 5.1 How edk2-rockchip makes DesignWare work for Windows
RK3588 has a 4 MiB DBI per controller, separate from a 256 MiB-aligned config window (`RK/RK3588/Include/Library/Rk3588Pcie.h:38-91`).

* **iATU** (`RK/RK3588/Library/Rk3588PciHostBridgeLib/PciHostBridgeInit.c:641-675`):
  * region 0 = CFG0 **with CFG shift mode** (CTRL2 bit 28, `:87,456-460`) for bus 1, 64 KiB (granule);
  * region 1 = CFG1 shift for bus 2+;
  * then IO and MEM.
* **MCFG modes, patched at EBS** (`RK/RK3588/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.c:207-392`):
  * **NXPMX6** (Windows default): FADT OEMID "NXPMX6" triggers a Windows quirk that filters duplicate devices on buses 0 and 1. Two MCFG entries: bus 0 at the DBI (root port visible) and bus 1+ in the CFG window. No bus offset (`:260-296`).
  * **GRAVITON** (Linux): MCFG OEM "AMAZON"/"GRAVITON" quirk (`:298-305`).
  * **SINGLE_DEV:** the root port is hidden (MCFG starts at bus 1, ends at bus 1). If the endpoint lacks CFG0 TLP filtering, the base is shifted by +0x8000 (one device), so the mirrored device appears once (`:317-378`). Filtering is probed at init (`PciHostBridgeInit.c:494-525`).
* **DSDT:** `PNP0A08`; `_BBN` and `_CRS` bus range from the patched names PBMI/PBMA/PBOF; `_CCA 0`; INTx `_PRT`; RES0 reserving
  DBI/CFG; an `_OSC` that never grants hot-plug, SHPC or LTR (`RK/RK3588/AcpiTables/Pcie.asl:11-135`).

### 5.2 QCS6490 PCIe0 is already ECAM-shaped
* **Address map** (`Q/Include/Qcs6490Pcie.h:21-35`, `kodiak.dtsi:2209-2227`):
  * DBI at **0x60000000**, which is 256 MiB aligned, so it is ECAM bus 0;
  * ELBI at 0x60000F20; iATU at 0x60001000;
  * "config" window **0x60100000** (1 MiB) = ECAM bus 1;
  * IO at 0x60200000 (1 MiB, unused); MEM at 0x60300000–0x63FFFFFF.
* This is exactly what Linux's DWC ECAM support needs: config space directly after the DBI and region 0 CFG0 with
  `PCIE_ATU_CFG_SHIFT_MODE_ENABLE` (`L/drivers/pci/controller/dwc/pcie-designware-host.c:420-459`, bit at
  `pcie-designware.h:181`).
* Qualcomm's own SC7280 Windows MCFG declares seg 0 = 0x60000000, buses 0–1 (`prior-art-acpi/.../MCFG.dsl:25-32`).
* **Aliasing inside the 2 MiB ECAM window:**
  * 00:00.0 = DBI (`0x000–0xF1C`), then ELBI.
  * 00:00.1 = **iATU registers** (0x60001000). Only probed if the root port's header type sets the multi-function bit, which a DWC root port does not.
  * 00:01.0–00:1F.7 = 0x60008000–0x600FFFFF: decode **unknown** (**INFERRED**; Linux never touches it, it returns NULL for bus 0, slot > 0, `pcie-designware-host.c:834-848`).
  * 01:xx = CFG0 TLPs. The uPD720201 answers every device number, so it would appear 32 times (our own observation, `Q/Library/Qcs6490PciSegmentLib/PciSegmentLib.c:17-19`).

### 5.3 Design
1. **iATU in ECAM mode** (Pcie0Init, replaces per-access retargeting):
   * Region 0: CFG0, shift mode, `0x60100000–0x60107FFF`: bus 1 device 0, all eight functions.
   * Region 2: CFG1, shift mode, `0x60108000–0x601FFFFF`. Accesses to 01:01–01:1F become Type 1 requests, which an endpoint must
     answer with UR (PCIe base spec), so they read all-ones instead of mirroring. That removes the need for any Windows quirk.
     **INFERRED:** verify that UR on a config read completes as 0xFFFFFFFF on this core (the RRS setting at
     `Q/Library/Qcs6490PciHostBridgeLib/Pcie0Init.c:1049-1060` suggests the AMBA error-response logic is configurable).
   * Region 1: MEM, as today (`Pcie0Init.c:1036-1042`).
   * Optionally move the iATU out of the ECAM window: `PARF_ATU_BASE_ADDR` is programmable (`Pcie0Init.c:541-542`). For example,
     put it at 0x60200000, the unused IO window, and update `PCIE0_ATU_BASE`. Linux reprograms it from its DT on probe
     (`L/drivers/pci/controller/dwc/pcie-qcom.c:383-403`).
   * PciSegmentLib becomes plain ECAM, `0x60000000 + (B<<20 | D<<15 | F<<12 | R)`. It keeps the guards: bus 0 slot > 0 reads
     all-ones, as Linux does; link down or controller unpowered reads all-ones (`Qcs6490Pcie.h:113-153`).
2. **MCFG:** seg 0, base 0x60000000, buses 0–1.
   * If the 00:01–00:1F probe in 5.4 shows aliasing or an SError, fall back to RK's NXPMX6 trick at EBS for Windows: FADT OEMID
     "NXPMX6" (`RK/.../AcpiPlatformDxe.c:294`). Our topology is exactly buses 0 and 1, the only ones recent Windows filters
     (`:263-264`).
   * Last resort: SINGLE_DEV with MCFG bus 1 only.
3. **DSDT `PCI0`:**
   * `_HID PNP0A08`, `_CID PNP0A03`, `_SEG 0`, `_BBN 0`, `_UID 0`, `_CCA 0` (UEFI treats PCIe DMA as non-coherent, `Q/QCS6490.dsc.inc:695-705`).
   * `_CRS`: WordBusNumber 0–1; DWordMemory 0x60300000–0x63FFFFFF, non-prefetchable, translation 0 (`Q/Library/Qcs6490PciHostBridgeLib/PciHostBridgeLib.c:68-83`); no IO.
   * `_PRT`: device 0xFFFF, pins 0–3 → GSIV **181, 182, 183, 184** (SPI 149–152, level-high, `kodiak.dtsi:2249-2252`). The endpoint INTA swizzles through the root port at 00:00 to pin 0.
   * `_OSC`: copy RK's mask.
   * `RES0` (PNP0C02): 0x60000000/0x200000 (ECAM, including the DBI and iATU), PARF 0x01C00000/0x3000, PHY 0x01C06000/0x1000.
   * Hot-plug capability is already cleared and ASPM is left off (`Pcie0Init.c:6-26`).
4. **Interrupts.** INTx first; Linux on this board uses DWC iMSI, so INTx delivery is untested (5.4). The GIC MBI MSI frame
   (2.2.4) is the Qualcomm-proven path to MSI later; ITS is absent. The "msi0..7" DWC iMSI lines are not usable by Windows.
5. **Keep at EBS:**
   * the link, PERST# high, power GPIOs 86/7/136 (`P/RubikPi3.dsc:83-85`), PCIe RPMh/BCM votes and clocks: nothing is undone today (`Pcie0Init.c:28-30`);
   * the Renesas firmware, which survives xHCI HCRST while powered (README "USB"). **INFERRED:** it may not survive a Windows secondary-bus reset or D3cold; we provide no `_PR3`, so no D3cold;
   * SMMU SID 0x1C00/mask 1 kept at EL1 (section 8).
6. **SMMU/IORT.** No IORT in phase 1. Qualcomm's IORT maps RIDs to SMMU streams for its own `qcsmmu` driver
   (`prior-art-acpi/.../IORT.dsl:187-235`). Without that driver it only matters as a DMA "memory size limit" hint (36 bits there).

### 5.4 Pre-ACPI experiments (UEFI shell, `mm`)
* Program the shift-mode regions, then read:
  * 0x60100000: Renesas VID 0x1912;
  * 0x60101000: function 1, expect FFFFFFFF;
  * 0x60108000: expect FFFFFFFF;
  * 0x60008000 and 0x600F8000: bus 0 aliasing test, watching for an SError.
* Boot Linux with `pci=nomsi` and confirm that xhci on 01:00.0 takes SPI 149 (`/proc/interrupts`). This validates INTx before Windows.

---------------------------------------------------------------------------------------------------

## 6. (e) USB under Windows

* **usb_2 (USB 2.0 Type-A):**
  * `Dwc3HostDxe` brings up power, clocks, PHY and the DWC3 in host mode (`Q/Drivers/Dwc3HostDxe/Dwc3HostDxe.c:1-60`). XhciDxe halts but does not reset the xHC at EBS. GCTL/PHY configuration stays, and Windows' HCRST resets only the xHC (**INFERRED**; RK3588 precedent: its DWC3 is described as plain `PNP0D10`, `RK/RK3588/AcpiTables/Usb3Host0.asl`).
  * ACPI `XHC0`: `_HID QCOM0AA1`, `_CID PNP0D15`, `_CCA 0`, MMIO 0x08C00000/0x100000, GSIV 274. Qualcomm's own SC7280 DSDT describes the same controller this way, plus wake interrupts 273/0x20D/0x20C we can omit (`prior-art.md` §5.1).
  * Must keep across EBS:
    1. SMMU SID 0xA0 bypass (EL1 only);
    2. the GDSC/clocks/PHY votes (L10C, L1C, L2B HPM). Nothing undoes them today (`Dwc3HostDxe.c:37-41`);
    3. GPIO119 high (`P/RubikPi3.dsc:91-92`).
  * **Risk: DMA above 4 GiB.** Under Gunyah, usb_2 DMA to a buffer at the top of DRAM timed out (`Dwc3HostDxe.c:27-36`). UEFI bounces below 4 GiB; Windows will not.
    * Hypothesis (**INFERRED**): the bypass context bank is set up like Linux's (`SCTLR=0`, CBAR `S1_TRANS_S2_BYPASS`, `SmmuDxe.c:242-243`; `L/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:497-506`), and its input address range or Gunyah's stage 2 may be limited.
    * Prefer EL2 for Windows (section 8). Test DMA above 4 GiB in UEFI first: temporarily allow DAC for usb_2/UFS, or write a 64-bit xHCI test.
* **Renesas uPD720201 (2× USB 3.0 + AX88179 Ethernet):**
  * Needs section 5 plus the firmware load already done by `RenesasXhciFwDxe`.
  * Windows binds inbox usbxhci by class `PCI\CC_0C0330` (`usbxhci.inf:38`). The AX88179 driver is inbox even in WinPE (`netax88179_178a.inf` is present in boot.wim image 2, `fwgap/bootwim.list`), so Setup has Ethernet.
* **usb_1 (Type-C).** Type-C/PD and role are handled by `pmic_glink` (ADSP firmware; `qcs6490-thundercomm-rubikpi3.dts:75-117`, orientation on GPIO140).
  * Windows needs Qualcomm's PmicGlink/UCSI/URS drivers and a running ADSP: out of scope for phase 1/2.
  * Later options:
    * host-only USB 2.0 mode forced in UEFI, as for usb_2 (VBUS control through the PMIC is unknown);
    * device mode through URS + ufxsynopsys: inbox `urssynopsys.inf` binds `ACPI\QCOM24B6` / `PNP0CA1`.
  * Keep EDL/adb behaviour for Linux.
* **Installer path.** Phase 1 should boot Setup from a stick on the **USB 2.0 port**: usb_2 needs no PCIe. Move to the USB 3.0
  ports when `PCI0` is validated. Q6A notes say USB devices must be connected before boot (CNX, Radxa docs). Expect similar
  hot-plug gaps where wake interrupts are missing.

---------------------------------------------------------------------------------------------------

## 7. (f) UFS under Windows

* **Driver:** inbox `storufs.sys` is in WinPE (boot.wim image 2). It binds `ACPI\QCOM24A5` with `FeatureFlags=0x40000000`,
  `HSSeries=2` (`fwgap/2/Windows/INF/storufs.inf:84,358-360`) or the generic `ACPI\CC_010901` (`:64`). Use `QCOM24A5` plus `_CLS`.
* **State at handoff:**
  * UfsPassThruDxe leaves the controller enabled and the link up. storufs re-does HCE and link start-up like any UFSHCI
    driver, which is what our edk2 driver does over the XBL-configured QMP PHY. HS gear switching relies on the PHY tables
    XBL programmed (**INFERRED**; Q6A reports "UFS works without drivers").
  * VCC/VCCQ rails, the reference clock and UFS clocks stay as XBL/UEFI left them. With no PEP, Windows cannot gate them.
* **SMMU.** SID 0x80. At EL1 the bypass entry must survive EBS for Windows (today it is restored, `SmmuDxe.c:255-269`), or the
  first Windows disk I/O faults. Gunyah resets the board on such faults, as the UEFI bring-up showed (README "SMMU"). At EL2: bypass.
* **DMA above 4 GiB.** storufs uses 64-bit DMA when CAP.64AS is set. Same risk and mitigation as usb_2 (section 6).
* **`_CCA`.** The DT says `dma-coherent` (`kodiak.dtsi:2487`) and Qualcomm's DSDT says `_CCA 1`, but both rely on SMMU attribute
  set-up. Start with `_CCA 0`, which is always safe; try 1 later.
* **LUN exposure.** storufs exposes every LUN as a disk. LUN 4 holds `uefi_a`, `logfs` (our variables!) and other firmware.
  Document "install to LUN 0 only". Windows cannot hide LUNs per ACPI.
  * Caveat: flange Linux lives on LUN 0 (rootfs/ESP), so Windows and Linux on UFS means repartitioning LUN 0.

---------------------------------------------------------------------------------------------------

## 8. (g) EL / Gunyah / SMMU strategy for Windows

| | EL1 under Gunyah (Snapdragon-laptop style) | EL2, Gunyah removed in SEC (recommended) |
|---|---|---|
| How | Hypervisor setting EL1 | Hypervisor EL2 + DSP Preload Disabled, so `GunyahExit = SEC` (`Q/Library/Qcs6490Lib/EarlyInit.c:282,608-650`) |
| SMMU | Gunyah owns it. Our bypass entries use the bypass context bank (`SmmuDxe.c:209-247`), and Gunyah aborts some writes (memory note: S2CR/CBAR). Windows has no SMMU driver, so **every DMA master Windows uses must keep a bypass entry**: UFS 0x80, MDSS 0x900/0x402, USB2 0xA0, PCIE0 0x1C00/1. | UEFI DMA already works with no entries ("not a Gunyah guest, UEFI's own streams left alone", `SmmuDxe.c:623-639`). Windows DMA passes the same way. |
| DMA above 4 GiB | Known failure for usb_2 (`Dwc3HostDxe.c:27-36`); cause unknown | Expected to work; test |
| Late exit at EBS | Not applicable | **Do not use for Windows.** Winload is switched to EL2 in mid-handoff (2.2.3) |
| Hyper-V / VBS | Impossible: Gunyah holds EL2. Qualcomm devices get EL2 for Hyper-V through Secure Launch / tcblaunch (TravMurav slbounce README); our TZ/Gunyah are not known to offer it (memory note: "not available on RUBIK's Gunyah") | Possible: Windows owns EL2. If VBS misbehaves, use `bcdedit /set hypervisorlaunchtype off` |
| DSPs | Could keep running under Gunyah (only useful with Qualcomm drivers) | Not runnable at EL2 (TZ refuses, README "DSPs at EL2") |
| Watchdog | APSS WDT is owned by Gunyah (`kodiak.dtsi:6897-6903`) | Gunyah stops its watchdog on exit (`GunyahExitDxe.c:164`); the WDT is free |
| Qualcomm drivers later | Expected environment for the qc* packages | They would need re-validation |

**Implementation.**
* **SmmuDxe.** `RestoreSmmu` consults `GetOsType()`. For Windows it leaves all entries in `mStreams` (`SmmuDxe.c:100-105`) in
  place. The ordering problem goes away because HandoffDxe identifies the OS before EBS events run.
* **PlatformConfigDxe.** Add an "OS profile" (Auto / Linux / Windows) that pre-selects EL2 + DSP Preload Disabled. SEC reads
  variables, so this works.
* **At EBS, when the boot is EL2-deferred and OS = Windows:** fall back to staying at EL1 with the entries kept, and print a warning.

---------------------------------------------------------------------------------------------------

## 9. (h) TPM, Secure Boot, RTC, watchdog and other runtime items

* **TPM.**
  * No path in phase 1. Qualcomm Windows devices use a TrEE fTPM: TPM2 table start method 9 at 0x808A4000 (`prior-art.md`
    §5.1), through `QcTrEE` (`ACPI\QCOMFFEC`, inbox in WinPE: `QcTrEE_i.inf:38`). That needs Qualcomm's Windows TZ apps, which
    our LE TZ set lacks (**INFERRED**; the Q6A LE flat build ships only `uefi_sec.mbn`).
  * Discrete SPI/I2C TPMs need SPB support that Windows lacks for this SoC.
  * Install with Setup's TPM/Secure Boot checks bypassed (LabConfig `BypassTPMCheck`/`BypassSecureBootCheck`, or Rufus options).
* **Secure Boot (optional).** Copy RK's `SECURE_BOOT_ENABLE` plumbing (`RK/Rockchip.dsc.inc:143-155,510-517`; FDF
  `ArmPlatformPkg/SecureBootDefaultKeys.fdf.inc`):
  * `AuthVariableLib`, `SecureBootVariableLib`, `SecureBootConfigDxe`, `SecureBootDefaultKeysDxe`, `PlatformPKProtectionLib`, `RuntimeCryptLib`.
  * Provide the MS KEK/db certificates at **build time from outside the repo** (`-D DEFAULT_KEYS=TRUE -D KEK_DEFAULT_FILE1=…`); they are Microsoft files and must not be committed.
  * Boot-time enrolment persists. Runtime dbx updates follow section 4.
* **RTC.**
  * New `Qcs6490PmicRtcLib` (RealTimeClockLib). PMK8350 RTC at SPMI SID 0, 0x6100 (RTC) / 0x6200 (alarm) (`pmk8350.dtsi:71-77`),
    through the PMIC arbiter at 0x0C440000 / channels 0x0C600000 / config 0x0C40A000 (`kodiak.dtsi:5809-5820`).
  * The board allows HLOS writes (`allow-set-time`, `qcs6490-thundercomm-rubikpi3.dts:919-923`).
  * Register the MMIO as `EFI_MEMORY_RUNTIME`.
  * For Linux boots, disable the RT RTC functions at EBS: RK pattern `RuntimeServicesManagerDxe.c:62-90`, and clear the
    GetTime/SetTime bits in our RT properties table. rtc-pm8xxx owns the device there.
* **Watchdog.** None for Windows in phase 1; GTDT has no SBSA watchdog. A WDAT table for 0x17C10000 is possible later, at EL2 only.
* **Other:**
  * CPU frequency: no Qualcomm PEP or cpufreq, so the cores stay at the frequency the firmware left. Optionally pin the
    EPSS/cpufreq-hw domains to a sane performance level before EBS (**INFERRED**; the LMh hardware still throttles).
  * Power button (PMIC PON): not described.
  * Shutdown and reboot go through PSCI.
  * Disable Fast Startup and hibernation at first. Runtime regions must sit at identical addresses across boots for resume;
    `PcdPrePiProduceMemoryTypeInformationHob` helps (`Q/QCS6490.dsc.inc:285`), but it is untested.

---------------------------------------------------------------------------------------------------

## 10. Phased implementation plan

1. **P0 – experiments (no Windows yet):**
   * the PCIe ECAM shift-mode tests (5.4);
   * INTx under Linux `pci=nomsi`;
   * DMA above 4 GiB for UFS and usb_2 at EL1 and at EL2;
   * whether 0xA0000000 survives a PSCI SYSTEM_RESET2 warm reset;
   * the GENI SE clock, to pick SPCR subtype 0x11 or 0x13.
2. **P1 – boot Setup from USB 2.0, install to UFS LUN 0 at EL2:**
   * ACPI modules and tables: FADT/MADT/GTDT/DSDT (CPU, UFS0, XHC0), SPCR/DBG2, PPTT, BGRT;
   * ConfigTableMode setting;
   * HandoffDxe with winload/winresume detection;
   * MdssDisplayDxe Keep mode;
   * NvStoreFvbDxe RAM runtime mode;
   * BDS Windows Boot Manager entry;
   * LabConfig bypass.
3. **P2:**
   * PCI0 + MCFG (Renesas USB 3.0, AX88179 Ethernet);
   * EL1 Windows mode in SmmuDxe;
   * PMK8350 RTC;
   * warm-reset variable carry-over;
   * SMBIOS serial/UUID;
   * optional MBI MSI frame.
4. **P3:**
   * runtime UFS flush in ResetSystem;
   * Secure Boot with external keys;
   * WDAT;
   * usb_1;
   * evaluation of Qualcomm driver packages (would need a Qualcomm-shaped DSDT with PEP0; see `prior-art.md` §7).

---------------------------------------------------------------------------------------------------

## 11. Open risks (highest first)
1. DMA above 4 GiB through Gunyah's bypass context bank. This is why EL2 is recommended.
2. Setup failing on SetVariable. Mitigated by P1 items c1 and c2.
3. Bus 0 aliasing at 0x60008000 and above, and UR → all-ones behaviour, for ECAM. Mitigated by the NXPMX6 / SINGLE_DEV fallbacks.
4. INTx delivery on PCIe0 is untested; Linux uses iMSI.
5. winload behaviour at EL2 (standard; RK/RPi do it) and never a mid-handoff EL switch.
6. Display votes surviving Windows idle (no `_LPI`).
7. Fast Startup / hibernation resume with a changing memory map.

---------------------------------------------------------------------------------------------------

## 12. Sources
* Local code: `edk2-qualcomm/…` and `edk2-rockchip/…` as cited; `edk2/ArmPkg/Library/PlatformBootManagerLib/PlatformBm.c`;
  `edk2/MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c`; `edk2/MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystem.c`.
* Linux 7.0.2: `arch/arm64/kernel/acpi.c:198-209`, `drivers/pci/controller/dwc/pcie-designware-host.c:420-536,834-858`,
  `pcie-designware.h:181`, `pcie-qcom.c:383-403`, `drivers/firmware/qcom/qcom_scm.c:2314-2345`,
  `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:488-506`. DTs: `devicetree/mainline/upstream/src/arm64/qcom/{kodiak.dtsi,qcs6490-thundercomm-rubikpi3.dts,pmk8350.dtsi}`.
* Board dumps: `workspace/usb-research/board/{dmesg_full.txt,live.dts}`, `workspace/usb-research/board-live.md:606-623`.
* Windows inbox INFs from the local 25H2 ISO `sources/boot.wim` image 2: `storufs.inf`, `usbxhci.inf`, `urssynopsys.inf`,
  `QcTrEE_i.inf`, `qcgpio_i.inf` (`QCOMFFEB`), `qci2c_i.inf` (`QCOMFFEA`), `tpm.inf`, `acpitime.inf`.
* Sibling reports: `prior-art.md` (SC7280 Windows ACPI facts, §5.1) and `q6a-firmware.md`.
* Microsoft, DBG2 spec (serial subtypes 0x11/0x13 SDM845): https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-debug-port-table
* Microsoft, BCDBoot options (NVRAM entry; `/s` relies on the fallback path): https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di
* Fallback loader behaviour of Windows: https://www.rodsbooks.com/efi-bootloaders/fallback.html
* Radxa Dragon Q6A Windows (what works without drivers): https://docs.radxa.com/en/dragon/q6a/other-system/windows ,
  https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/
* Secure Launch / EL2 on Qualcomm Windows devices: https://github.com/TravMurav/slbounce
* aarch64-laptops issue on runtime variable writes: https://github.com/aarch64-laptops/build/issues/25
