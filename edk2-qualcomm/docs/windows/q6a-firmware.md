# Radxa Dragon Q6A Qualcomm firmware and the Q8B Windows driver pack: ACPI and Windows findings

Scope: reverse-engineer the Radxa Dragon Q6A (QCS6490) Qualcomm UEFI and related images to find out whether they carry
ACPI/Windows support, and use the Radxa Dragon Q8B Windows driver pack to learn the ACPI device naming that Qualcomm's
Windows drivers expect. The goal is input for planning full Windows 11 ARM64 support on the RUBIK Pi 3.

Inputs:
* `<downloads>/dragon-q6a_flat_build_251013/flat_build/spinor/dragon-q6a/` (`uefi.elf`, `imagefv.elf`, `tools.fv`, `xbl_config.elf`, plus `xbl.elf`, `tz.mbn`, `hypvm.mbn`, `devcfg.mbn`, `contents.xml`)
* `<downloads>/dragon-q8b_win_driver_pack_v1.0.0/driver_pack/` (70 INFs)

Outputs (everything is under `workspace/windows-research/`, which is git-ignored):

| Path | What |
|---|---|
| `q6a-fw/fvparse.py` | PI FV/FFS/section extractor. It handles the ELF wrapper (scans for `_FVH`), FFSv2/v3, nested FV image sections, LZMA GUIDed sections (`EE4E5898-…`, `lzma.FORMAT_ALONE`), **Qualcomm gzip GUIDed sections (`1D301FE9-BE79-4353-91C2-D23BC959AE0C`)**, PE32/TE/RAW/UI/DEPEX/VERSION, and pad files |
| `q6a-fw/out/` | Every extracted section, laid out as `out/<image>/FV<n>/<FileGUID>_<idx>.<ext>`, plus `out/inventory.tsv` |
| `q6a-fw/inventory.md` | Human-readable inventory: 7 FVs and 170 FFS files, each with GUID, type, UI name and section types |
| `q6a-fw/named/` | The same modules, copied under their UI names (`FV<n>_<UiName>.efi` / `.cfg`) |
| `q6a-fw/dis/` | objdump disassembly of `DtPlatformDxe`, `EnvDxe`, `Cmd` and `UFSDxe` (the code paths cited below) |
| `q6a-fw/xbl_config/` | The two DTBs carved out of `xbl_config.elf`, decompiled to `.dts`, and `iort-summary.txt` |
| `q6a-fw/fdt_iort.py` | Dumps the IORT-in-DT node into a table |
| `q6a-fw/infparse.py`, `q6a-fw/q8b-inf-hwids.tsv` | INF parser and the full Q8B hardware-ID table (384 rows) |
| `q6a-fw/q8b-inf-7280-sections.txt` | SC7280-specific registry sections found inside the 8280 INFs |

No `q6a-acpi/` directory was created, because the firmware contains no ACPI tables to decompile (see section 2).

---

## 0. TL;DR

1. **This Q6A firmware (flat build 251013) has no ACPI support at all.**
   * There is no `AcpiTableDxe`, no `AcpiPlatformDxe` and no ACPI table storage file.
   * A checksum-validated scan of every decompressed FV and every image in the flat build found no DSDT, SSDT, FACP, APIC, GTDT, IORT, PPTT, MCFG, SPCR, DBG2, CSRT or TPM2 table.
   * It is the Qualcomm Linux (QLI) "LA/LE" build of `BOOT.MXF.1.0.c1`. It boots a DT through `DtPlatformDxe` and `LinuxLoader`.
   * It **cannot** serve as the source of ACPI tables.
2. **Radxa has since shipped a Windows-capable firmware.**
   * Files: `dragon-q6a_flat_build_wp_251215.zip` and `dragon-q6a_flat_build_wp_260120.zip`.
   * Per Radxa's release notes, the code base moved from `qualcomm-linux-spf-1-0_ap_standard_oem` to **`qcm6490-wp-1-0_ap_standard_oem`** (WP = Windows platform).
   * Windows 11 24H2/25H2 boots on it with a "6490" test-signed driver pack.
   * **That WP firmware is the real candidate base for our ACPI tables.** We did not download it (rules), but `fvparse.py` should extract it unchanged.
3. Code left over in the LA build still documents **how Qualcomm firmware chooses between ACPI and DT and sets the EL level**. See section 3:
   * the `OsConfigTableSelection` variable: 0 = ACPI (Windows, the default), 1 = DT, 2 = DT with EL2
   * `DtAcpiPref`
   * `OsTypeString` (`LA` or `WP`)
   * the `SecurityFlag` bits for TrEE/fTPM and winsecapp
   * the `WinAcpiUpdate` protocol and the `QUFN` DSDT patching
4. **`xbl_config.elf` holds Qualcomm's IORT for Kodiak, encoded in DT form** (`/soc/iort`: 2 SMMUv2 nodes and 21 named components with SID/mask mappings). It is directly reusable for our IORT table (section 4).
5. The Q8B pack shows the Windows naming scheme:
   * IDs are `ACPI\QCOMxxxx`, with compatible IDs `ACPI\VEN_QCOM&DEV_xxxx&SUBSYS_<board>` and `&REV_xxxx` (GPU), plus child bus IDs such as `URS\…&HOST/FUNCTION`, `ADSP\…`, `ADCM\…` and `AUCD\…`.
   * The 8280 IDs fall in the `QCOM06xx` range, with a few shared older ones (`QCOM04DD` SCM, `QCOM04DE` TrEE, `QCOM0427` ACPI bridge).
   * The Radxa platform driver matches **`ACPI\RAXA1001` (commented "6490")** and `RAXA1002` ("8280").
   * Some 8280 INFs already contain unused **`*_7280` register sections** with SC7280 SMMU SIDs (section 5).

---

## 1. Firmware identity and layout

* `contents.xml:30`: `<hlos_type cmm_var="HLOS_TYPE">LE</hlos_type>`.
* `contents.xml:87`: boot build `BOOT.MXF.1.0.c1-00364-KODIAKLA-1`. TZ is `TZ.XF.5.29.1-00084-KODIAKAAAAANAAZT-1`, and the TZ apps are `TZ.APPS.1.29-00208` (only `uefi_sec.mbn`).
* Build paths embedded in the binaries (for example in `named/FV3_DtPlatformDxe.efi` and `named/FV2_QcomBds.efi`):
  `/home/ubuntu/qcWorkspace/fw/bsp_mirror/qualcomm-linux-spf-1-0_ap_standard_oem_nomodem/BOOT.MXF.1.0.c1/boot_images/Build/KodiakLAA/Core/RELEASE_CLANG140LINUX/...`
  The **`KodiakLAA`** target is the LA variant. The `TzDxeLA` driver name follows the same pattern.
* `uefiplat.cfg` (FFS `DDE58710-41CD-4306-DBFB-3FA90BB1D2DD`, saved as `named/FV1_uefiplat.cfg`):
  * line 67: `PlatConfigFileName = "uefiplatLA.cfg"`
  * line 68: `OsTypeString = "LA"`
  * line 139: `DefaultBDSBootApp = "LinuxLoader"`
  * line 189: `EnableACPIFallback = 0x0` ("Allow individual ACPI tables loading")
  * line 126: `SecurityFlag = 0x44`, with the legend on lines 127-135: `TreeTpmEnableFlag=0x2`, `VariableServicesFlag=0x10`, `WinsecappFlag=0x20`, `LoadSecAppFlag=0x40`, …

FV layout (full list in `q6a-fw/inventory.md`):

| FV | Source | Content |
|---|---|---|
| FV1 | `uefi.elf` @0x1000, 0x480000 | SEC (TE), `uefiplat.cfg`, and two FV_IMAGE files compressed with **gzip** |
| FV2 | FFS `9E21FD93-…`, 0x55A000 | DxeCore, Pcd, Env, Runtime, Variable (+Emu), Gic, Timer, ChipInfo, PlatformInfo, DALSys, HALIOMMU, HWIO, Clock, Sdcc, I2C, SPMI, TLMM, Qup, Pmic, UFS, SpiNor, CmdDb, PwrUtils, Rpmh, Npa, ULog, Vcs, ICB, Smem, **QcomBds**, UiApp, BootManagerMenuApp, Shell, `BDS_Menu.cfg`, `SecParti.cfg`, `uefipil.cfg`, `default-upstream-dtb.dtb` (model "Radxa Dragon Q6A") |
| FV3 | FFS `D14C51ED-…`, 0x4A1000 | Console, CPR, **DtPlatformDxe**, Esrt/Fmp, Gpi, SPI, HSUART, FeatureEnabler, SoftSKU, NvmExpress, Buttons, IPCC, Glink, PmicGlink, UsbPwrCtrl, Tsens, Limits, DDRInfo, PciBus, PciHostBridge, UsbfnDwc3, XhciPciEmulation, Xhci, UsbBus/Kb/MassStorage/Msd/Device/Config/Init, UCDxe, Rng, Crypto, **PILDxe**, PILProxyDxe, SecRSA, Smbios, SmBiosTable, FvDxe, ParserDxe, SerialPort, SecurityStub, SecurityDxe, `logo1.bmp`, 22 panel XMLs (including `Panel_lt9611uxc_dsi2hdmi_vid.xml`), Ebl |
| FV4/5 | `imagefv.elf` | LZMA, containing `logo_custom.bmp` |
| FV6/7 | `tools.fv` | LZMA. Apps: Cmd, ListVars, Menu, Pgm, RPMBProvision/Erase, DelBootVars, UsbfnMsdApp, SecurityToggleApp, DebugPolicyToggleApp, FeatureEnablerToggleApp, CapsuleApp, Ebl, Mptest, Shell, **WinDsdtUpdateControllerUI**, MemoryProfileInfo, Fastboot. Menu cfgs: `USB_Menu`, `Uefi_Menu`, `Pmic_Menu`, `Config_Menu`, `EUD_Menu`, `Clock_Menu`, `DriverList` |

There is no display DXE in this build. Only the panel XMLs and the HII "DisplayEngine" are present. Radxa's 260120 release
notes call "HDMI output now functional in UEFI" a new feature, which fits.

## 2. ACPI presence check (negative)

* **Driver GUID scan** (`q6a-fw/out/**/*.fv`). The search covered `AcpiTableDxe` `9622E42C-…`, the ACPI table storage file `7E374E25-…`, `gEfiAcpiTableProtocolGuid`, `gEfiAcpiSdtProtocolGuid`, `gEfiAcpi10/20TableGuid` and `gEdkiiPlatformHasAcpiGuid`. The only modules that reference these GUIDs are:
  * Shell (`acpiview`)
  * **DtPlatformDxe** (`PlatformHasAcpi` and `FdtTable`)
  * **UsbConfigDxe** (`AcpiSdtProtocol`)
  * SecurityStubDxe (Tcg2/TrEE)
  * EnvDxe (`FdtTable`)

  Nothing in the build produces ACPI tables.
* **Table scan.** Every 4-byte signature that has a plausible length and a zero byte checksum was checked. This covered every decompressed FV and every file in the flat build (excluding the 9.6 GB OS image). Result: **0 hits**.
* **Other images:**
  * `hypvm.mbn` (Gunyah) has a string `acpi_bring_up` (offset 0xd5ce1). It is unrelated: it sits next to IFE/PIL strings.
  * `xbl.elf` has `SC_KODIAK_WINDOWS` (offsets 0x83d74 and 0xa1914). It is only a ChipInfo part-name table entry, alongside `SC_KODIAK_CHROME` and `QCS6490`.

## 3. How Qualcomm's firmware chooses ACPI vs DT, EL1 vs EL2, and LA vs WP

### 3.1 `OsConfigTableSelection` (vendor GUID `gQcomTokenSpaceGuid` = `882F8C2B-9646-435F-8DE5-F208FF80C1BD`)

* **Menu entry.** `Uefi_Menu.cfg:223-228` (`named/FV7_Uefi_Menu.cfg`): "Select OS Config Table: ACPI (Windows, Default) Vs DT (Linux) Vs DT w/ EL2 (Linux w/ KVM)" → `App = Cmd`, `Arg = "SelectOsConfigTable"`.
* **Cmd app** (`dis/Cmd.S` 0x5250-0x5350; strings at 0x14ce3: `" 0 : ACPI (Windows, Default)"`, `" 1 : DT (Linux)"`, `" 2 : DT w/ EL2 (Linux w/ KVM"`). It calls `GetVariable(L"OsConfigTableSelection", gQcomTokenSpaceGuid)` with size 1, prompts for 0..2, and calls `SetVariable` with attributes 7 (NV|BS|RT). The GUID is at Cmd .data 0x19038.
* **EnvDxe** (`dis/EnvDxe.S` 0x1608-0x16d8):
  1. It reads the variable as a UINT32.
  2. If the variable is absent, it falls back to the XBL-config DT property `/sw/uefi/uefiplat/OsConfigTableSelection`, read via the DTBExtn protocol `0389B776-625F-11EB-83BE-C741A913DE34`. This build sets the property to **`<0x01>`** (`xbl_config/dtb_037e38.dts:3868`).
  3. It sets a flag = (value == 2).
* **Radxa addition** (`EnvDxe` 0x1b34-0x1b98). The `/chosen` property `radxa,enable-kvm` in the OS DTB overrides that flag: "enable-kvm is set in DTB, booting with EL2".
* **Hypervisor SMC** (`EnvDxe` 0x16dc-0x1800). EnvDxe issues **SMC `0x02000121`, param-id `0x23`, args {0,0,flag}**:
  * flag = 1: "Non-Gunyah based bootup". It first calls ArmLib helpers that read the current-EL MMU registers and do cache/TLB maintenance (EnvDxe 0x40c4-0x4644).
  * flag = 0: "Gunyah based bootup".

  This is exactly the call our `GunyahExitDxe` makes (`edk2-qualcomm/Silicon/Qualcomm/QCS6490/Library/Qcs6490Lib/Qcs6490Helper.h:11-13`, `Drivers/GunyahExitDxe/AArch64/GunyahExit.S:5,34-36`).

  **Consequence (INFERRED for WP builds):** in Qualcomm's own design, the ACPI/Windows choice (0) keeps Gunyah, so Windows runs at EL1 as a Gunyah guest. Only "DT w/ EL2" leaves Gunyah.

### 3.2 `DtAcpiPref` (EDK2 `DtPlatformDxe`, Qualcomm-modified)

* The variable is `L"DtAcpiPref"` under `gDtPlatformFormSetGuid` `2B7A240D-D5AD-4FD6-BE1C-DFA4415F5526` (`DtPlatformDxe` .data 0xb028).
* Code: `dis/DtPlatformDxe.S` 0x1404-0x1600.
* The PCD `PcdDefaultDtPref` = **1** (byte at file offset 0x83a4), so DT is the default.
* With `Pref = 1` (ACPI), the driver installs `gEdkiiPlatformHasAcpiGuid` (`F0966B41-…`, .data 0xb048) and loads no DTB. With `Pref = 0`, it loads and authenticates the DTB (`\combined-dtb.sig`, `\secondary-dtb.sig`, fallback `default-upstream-dtb.dtb`) and installs it as the FDT configuration table.
* Qualcomm's upstream-style design therefore gates `AcpiTableDxe`/`AcpiPlatformDxe` on `gEdkiiPlatformHasAcpiGuid`. In this LA build nothing consumes that GUID, so selecting ACPI would yield neither a DT nor ACPI tables.
* This is the same DT/ACPI mechanism as edk2-rockchip's ConfigTableMode (INFERRED equivalence).

### 3.3 `OsTypeString` ("LA" vs "WP")

* `UFSDxe` (`dis/UFSDxe.S` 0x3b84-0x3c70; strings at 0x18a1f and 0x18a2c "WP") reads `OsTypeString` from `uefiplat.cfg`. When the value is **"WP"**, it registers an extra UFS sleep/ExitBootServices callback before checking `UEFIExitUfsSSURequired`.
* `SdccDxe` does the same comparison (strings at 0x14903 and 0x14910 "WP").
* The SD/UFS storage-security path also checks `OsTypeString` ("LA" at 0x1a6c4) before loading the `storsec` TZ app.
* So a WP build differs in the storage/RPMB listener behaviour at ExitBootServices (INFERRED: UEFI variables and fTPM in RPMB are kept serviceable for Windows).

### 3.4 Other Windows-specific hooks found in the LA build

* **`WinDsdtUpdateControllerUI`** (tools.fv, `A9F19C13-4DF7-3952-6811-F6B4479B3C0C`). It needs a "**WinAcpiUpdate** protocol" (`gWinAcpi->SetDefectivePartsVariable`); the protocol is absent here. `DriverList.cfg:19-32` offers "Disable Bin B - GPU" (`SetDefectiveBitmask 6`) and "Disable Bin D - NSP0" (`SetDefectiveBitmask 16`).
  INFERRED: the WP firmware includes a `WinAcpiUpdate` DXE that patches the DSDT for fused-off (defective) GPU/NSP parts.
* **`UsbConfigDxe`** (FV3 `CD823A4D-…`).
  * `UsbConfigUpdateACPIEntry` and `UpdateDsdtTable` use `gAcpiSdt->FindPath` and set the value of the DSDT name object **`QUFN`** (string at 0x11a4f).
  * This is driven by `USB_Menu.cfg:78-80` "Enable QcUsbFn HLOS driver for primary port" (variable `UsbfnEnableQcFnHLOS`).
  * So the Qualcomm Windows DSDT contains `Name(QUFN, …)`, which selects the Qualcomm vs Microsoft USB function stack.
* **`HALIOMMU`** builds its SMMU setup from the IORT-in-DT (`parseIORTDT`, `NumberofIORTNodes`); see section 4.
* **Debugging and security hooks:**
  * `Cmd`/`Uefi_Menu.cfg:153-155` provide "Enable high speed UART for Windbg (3Mbps)".
  * `SecurityFlag` documents `TreeTpmEnableFlag` (fTPM via TrEE) and `WinsecappFlag`; this build has both off (0x44).
  * `tz.mbn` has `InvokerIsFTPM`; `devcfg.mbn` has `qcom.tz.tpm` / `tpm_type*` config.
  * No fTPM TA image is in the flat build.
  * INFERRED: Qualcomm fTPM for Windows needs WP TZ apps (and `SecurityFlag |= 0x2|0x20`) that this LE flat build lacks.
* **`QcomBds`** boots `DefaultBDSBootApp` or standard `Boot####` and `\EFI\BOOT\BOOTAA64.EFI` options. Nothing Windows-specific beyond this was found.

## 4. Qualcomm IORT for Kodiak (from `xbl_config.elf`)

* `xbl_config.elf` LOAD segment 9 (file offset 0x37e38) is the post-DDR XBL config DTB. The pre-DDR one is at 0x34408.
* Node `/soc/iort` (`xbl_config/dtb_037e38.dts:1570`) is an ACPI IORT header encoded as DT properties:
  * `Signature = "IORT"`
  * `OEMID "QCOM"`
  * `OEMTableID "QCOMEDK2"`
  * `OEMRevision 0x8998`
  * `NumberofIORTNodes = 0x17`
* Decoded node list: `xbl_config/iort-summary.txt` (produced by `fdt_iort.py`).

SMMU nodes:

| SMMU node | Base | Span | Model | Global IRQ (GSIV) | Context IRQs |
|---|---|---|---|---|---|
| `APPS_MMU500_SMMU_APP` | 0x15000000 | 0x80000 | 3 (MMU-500) | 97 | 80 IRQs from 0x80 |
| `GPU_GFX_MMU500_SMMU_GFX` | 0x3DA0000 | 0x10000 | 3 | 705 | 10 IRQs from 0x2C6 |

The interrupt values are GSIVs: kodiak.dtsi gives `GIC_SPI 65` (65 + 32 = 97) at line 6717 and `GIC_SPI 673` (673 + 32 = 705) at line 3359.

Named components (`DevObjectName`, 36-bit address size). Each mapping's `OutputBase` = `(SMR mask << 16) | SID`, which matches the DT `iommus` cells:

| Component | SID / mask |
|---|---|
| `CRYPTO` | 0x4e4/0x11, 0x4e6/0x11, 0x4f2, 0x4f3, 0x4f8/1, 0x4fc/1, 0x4fe, 0x4ff |
| `CAMERA` | 0x800/0x4e0, 0x2000/0x20, … |
| `DISPLAY` | 0x900/0x402, 0x901, 0xd01 |
| `GPU` (GFX SMMU) | 0-7/0x400 |
| `IPA` | 0x480-0x484 |
| `LPASS` | 0x1826 |
| `LPASS_ADSP` | 0x1801-0x1807, 0x180f |
| `NSP` | 0x1181-0x118f/0x420 |
| `PCIE0` | 0x1c00/1, 0x1c04/3, …, 0x1c40/0x3f |
| `PCIE1` | 0x1c80/1, … |
| `QDSS` | 0x4a0, 0x4c0 |
| `QUP0` | 0x123, 0x136 |
| `QUP1` | 0x43, 0x56 |
| `SDC1_EMMC` | 0xc0 |
| `SDC2` | 0x100 |
| `SDC4` | 0x60 |
| `UFS_MEM` | **0x80** |
| `USB2` | **0xa0** |
| `USB3` | **0xe0** |
| `VIDEO` | 0x2180/0x20, … |
| `ECATS_TEST` | 0, 1 |

The SIDs were cross-checked against `devicetree/mainline/upstream/src/arm64/qcom/kodiak.dtsi`: UFS 0x80 (line 2486), usb_2 0xa0 (4398), usb_1 0xe0 (4955), sdhc_1 0xc0 (1031), sdhc_2 0x100 (4233), and PCIe 0x1c00/0x1c80 (2169, 2427).

Notes for our IORT (INFERRED):
* The Windows IORT would use the same SMMU nodes. The named-component `DevObjectName`s must be the ACPI paths of our DSDT devices (for example `\_SB.UFS0`), not these logical names.
* The `InputBase` values 0x0300000N are Qualcomm-internal IDs.
* Windows' `qcsmmu` driver also loads an `SMMC.bin` resource and registry parameters (`qcsmmu8280.inf`: `HYPD Enabled=0`, `MMUV Type=0` = MMU-500).

## 5. Radxa Dragon Q8B Windows driver pack: hardware IDs

70 INFs were parsed (`q6a-fw/q8b-inf-hwids.tsv`). Most are dated 12/15/2025 with version 1.0.4498.4500. The pack has no
UFS, SDHC, PCIe root-port or xHCI core drivers; those are in-box Windows drivers.

**Naming conventions:**
* The primary HID is `ACPI\QCOMxxxx`.
* Board-specific extension INFs (Class=Extension) match the compatible form `ACPI\VEN_QCOM&DEV_xxxx&SUBSYS_<CDP0|MTP0|QRD0|QRDR>8280`. This means the DSDT `_SUB` value carries the board ID (QRD08280, CDP08280, …).
* The GPU adds `&REV_18xx`.
* Child devices are enumerated by Qualcomm bus drivers, not ACPI: `URS\QCOM068B&HOST` / `&FUNCTION` (USB role-switch children), `ADSP\QCOM0622`, `ADSP\QCOM060F`, `ADCM\QCOM06C1`, `AUCD\QCOM0629` and `QCA_SHB\UART_H4*`.

| HW ID(s) | Driver (folder) | Function | SC7280 block / DT node (kodiak.dtsi line) |
|---|---|---|---|
| `ACPI\QCOM0427` | qcabd | Qualcomm ACPI Bridge Device | none (ACPI/PEP helper, INFERRED) |
| `ACPI\QCOM04DD` | qcscm | System Manager SCM (TZ SMC) | `firmware/scm` (721) |
| `ACPI\QCOM04DE` (+`VEN_QCOM&DEV_04DE&SUBSYS_*8280` ext) | QcTrEE / QcTreeExtQcom8280 | TrEE (QSEE apps, fTPM front-end) | TZ |
| `ACPI\QCOM0604` | HalExtQCWdogTimer8280 | HAL extension, APSS watchdog | watchdog@17c10000 (6897) |
| `ACPI\QCOM0609` | qcsmmu8280 | System MMU (with `SMMC.bin`) | apps_smmu@15000000 (6711), adreno_smmu@3da0000 (3353) |
| `ACPI\QCOM060A` | qckmbam8280 | BAM bus | cryptobam@1dc4000 (2589), other BAMs |
| `ACPI\QCOM060B` | qcspmi8280 | SPMI bus | spmi@c440000 (5809) |
| `ACPI\QCOM060C` | qcgpio8280 (`qcgpio.sys`) | TLMM GPIO | pinctrl@f100000 (5827) |
| `ACPI\QCOM060D` | qcipcrouter8280 | IPC router | GLINK/SMEM |
| `ACPI\QCOM060E` | qcspi8280 | QUP SPI | geniqup@9c0000 (1101) / @ac0000 (1616) |
| `ACPI\QCOM0610` (+`SUBSYS_QRD08280`) | qci2c8280 | QUP I2C | same QUP wrappers |
| `ACPI\QCOM0611` | qcadc8280 | PMIC ADC | PMIC via SPMI |
| `ACPI\QCOM0613` | qcdiagrouter8280 | Diag router | none |
| `ACPI\QCOM0616` | qcuart8280 | QUP UART | QUP wrappers |
| `ACPI\QCOM0617` (+`SUBSYS_QRD08280`) | qcpep.wd8280 (`qcpep8280.sys`, `PPMSettings-8280Profiles.wd.ppkg`) | **Power Engine Plug-in** | RPMh rsc@18200000 (6963), AOSS QMP@c300000 (5792), cpufreq@18591000 (7046) |
| `ACPI\QCOM0637`-`0655`, `0658`-`065A`, `0690`-`0692`, `06B1`-`06C0`, `06C4`-`06C6`, `06D6` | qcpep | Temperature sensor devices (PEP-owned) | tsens0@c263000 (5764), tsens1@c265000 (5775) |
| `ACPI\QCOM0657` | qcpep | Battery current limit monitor | PMIC |
| `ACPI\QCOM065D`-`0664` | qcpep | ADC temperature monitor | PMIC ADC-TM |
| `ACPI\QCOM06C8`-`06CB` | qcpep | PMIC die temperature alarm | PMIC |
| `ACPI\QCOM06D7` | qcpep | MMRM policy device | none |
| `ACPI\QCOM061B` | qcsubsys8280 (+ext_adsp) | Audio DSP subsystem | remoteproc@3700000 (4436) |
| `ACPI\QCOM06B0` | qcsubsys8280 (+ext_cdsp) | Compute DSP subsystem | remoteproc@a300000 (4761) |
| `ACPI\QCOM061F` | qcsubsys8280 | Sensor subsystem | n/a on 7280 (sensors on ADSP, INFERRED) |
| `ACPI\QCOM0620` | qcsubsys8280 | Subsystem dependency device | none |
| `ACPI\QCOM068D` | qcsubsys8280 (+ext_spss) | Secure processor subsystem | n/a on 7280 (INFERRED) |
| `ACPI\QCOM06C3` (`SUBSYS` only) | qcsubsys_ext_cdsp | NSP0 CDSP software thermal | none |
| `ACPI\QCOM062B`, `ACPI\QCOM06D3` | qcpmic8280 | PMIC framework, "PML0" | PMICs (pm7325, pmk8350, …) |
| `ACPI\QCOM062C` | qcpmicapps8280 | PMIC apps (power key, …) | PMIC |
| `ACPI\QCOM062D` | qcpmicgpio8280 | PMIC GPIO | PMIC |
| `ACPI\QCOM068E` | QcPmicGlink8280 | PMIC GLINK (battery / Type-C via ADSP) | `pmic_glink` |
| `ACPI\QCOM062F` | qccdi8280 | Crash dump injector | none |
| `ACPI\QCOM0636` (+`REV_1800`…`REV_182A`, `SUBSYS_QRD08280`) | qcdx8280 (`qcdxkm8280.sys` + UMDs; `qcdxkmsuc8280.mbn`, `qcvss8280.mbn`) | Adreno GPU + display + video | gpu@3d00000 (3198), mdss@ae00000 (5351), venus@aa00000 (4988) |
| `ACPI\QCOM065C` | qcadsprpc8280 | FastRPC | none |
| `ACPI\QCOM0682` | qcadsprpcd8280 | Audio RPC daemon | none |
| `ACPI\QCOM0683` | qcsyscache8280 | System cache (LLCC) | system-cache-controller@9200000 (4725) |
| `ACPI\QCOM0684` | qcglink8280 | GLINK shared-memory port | SMEM + IPCC |
| `ACPI\QCOM06C2` | qcipcc8280 | IPCC interrupt controller | mailbox@408000 (993) |
| `ACPI\QCOM0687` | qcsp8280 | Secure Processor | n/a on 7280 (INFERRED) |
| `ACPI\QCOM0688` | qcgpi8280 | GPI DMA bus | dma-controller@900000 (1079), @a00000 (1594) |
| `ACPI\QCOM068F` (+ext `SUBSYS_*8280`) | qciommu / qciommuext8280 | IOMMU (DMA remapping) | consumes IORT |
| `ACPI\QCOM0696` | qcppx8280 | PCIe platform extension plug-in (PEP for PCIe) | pcie@1c00000 (2209), pcie@1c08000 (2338) |
| `URS\QCOM068B&HOST`, `URS\QCOM068C&HOST` | QcXhciFilter8280 (LowerFilter on in-box `usbxhci.inf`) | xHCI host via the URS role switch | usb_1@a600000 (4911) and usb_2@8c00000 (4358). Parent HIDs `QCOM068B/068C` are URS devices (in-box URS driver, INFERRED) |
| `URS\QCOM068B&FUNCTION`, `URS\QCOM068C&FUNCTION` | QcUsbFnSsFilter8280 | USB device (function) mode | same controllers |
| `ACPI\QCOM06A1` | QcXhciFilter8280 | Host-only multiport xHCI | 8280 multiport; no 7280 equivalent (INFERRED) |
| `ACPI\QCOM06A4` (+`SUBSYS_QRDR8280`) | qcusbcucsi8280 | USB Type-C UCSI | via pmic_glink |
| `ACPI\QCOMFFE1` | qcursext | URS extension | none |
| `ACPI\QCOM06A8` | qcconnectionsecurity8280 | Connection security | none |
| `ACPI\QCOM06AC` | QcSkExt8280 | Secure Kernel extension (VBS) | needs EL2 / Hyper-V (INFERRED) |
| `ACPI\QCOM06D0` (+`root/WLDS`) | qcWlanSleepMgr8280 | WLAN sleep manager | none |
| `ACPI\QCOM06D8` | qcSSGServicesUMD | SSG secure services | none |
| `ACPI\QCOM06DC` | QcTftpKmdf | TFTP (remote file system for DSPs) | none |
| `ACPI\QCOM06DD` | QcSOCPartition | SoC partition interface | none |
| `ACPI\QCOM06DE` (+`SUBSYS_*7180`/`8180`) | QCDiagBridge | Diag bridge | none |
| `ACPI\QCOM06DF` | qcpdsr | Protection-domain service registry | none |
| `ACPI\QCOM06E0` | qcpil (+qcpilEXT8280, qcpilfilterext) | Peripheral Image Loader | DSP firmware loading |
| `ACPI\QCOM06E1` | qcrpen | Reset/power error notifier | none |
| `ACPI\QCOM06E4` | qcsecapp | Secapp (UEFI variables via TZ) | none |
| `ACPI\QCOM06E5`, `ACPI\QCOMFFE0` | qcSubsysThermalMgr | Subsystem thermal mitigation | none |
| `ADSP\QCOM0622` (+`SUBSYS_QRD08280`) | qcadcm8280, qcacsp_qrd8280 | Audio DSP and calibration manager | LPASS |
| `ADSP\QCOM060F` | qcslimbus8280 | SLIMbus | LPASS |
| `ADCM\QCOM06C1` | qcaucd8280 (+ext) | Aqstic audio codec | LPASS codecs (rx/tx/va macros 3200000 …) |
| `AUCD\QCOM0629` | qcaudminiport_Base8280 (+QRD ext, APO) | Audio adapter miniport | none |
| `ACPI\VEN_QCOM&DEV_0C6B&SUBSYS_*` | qcbluetooth8380 | Bluetooth UART transport | n/a (WCN685x/WCN7850) |
| `USB\VID_0CF3…`, `QCA_SHB\UART_H4*` | BtFilter8141, qcbtaddvscregistry8380 | Bluetooth | n/a |
| `PCI\VEN_17CB&DEV_1103/1107` | qcwlanhsp8141, qcwlanhmt8380 | PCIe Wi-Fi (FastConnect 6900/7800). INF strings mention `QcWlan.DeviceDesc.6490` | RUBIK Pi 3 Wi-Fi is the on-die WPSS (wifi@17a10040, 2166), not PCIe, so no match |
| `PCI\VEN_1179&DEV_0220` | tc956x | Toshiba TC956x PCIe Ethernet (Q8B board) | n/a |
| **`ACPI\RAXA1001`** ("; 6490"), `ACPI\RAXA1002` ("; 8280") | radxaplatform | "Radxa WoS Platform Device" (`radxaplatform.inf:26-29`) | Radxa vendor device in Q6A's DSDT |

**SC7280 data inside the 8280 INFs** (`q8b-inf-7280-sections.txt`):
* `qcadsprpc8280.inf:215-258` `[ARPCReg_7280]`:
  * ADSP FastRPC SIDs 0x1803-0x1807 (CB 0x26-0x28, 0x36, 0x37; ARIDs 0x17030034-38)
  * CDSP SIDs 0x1181-0x118F (CB 0x19-0x25)
  * remote heap 0xC6700000/0x600000; dynamic loading 0xC6500000/0x200000
* `qcadcm8280.inf:83-104` `[ADCMReg_7280]`: `AudioAridBase` 0x07030000; ADSP ML carve-out 0x81800000-0x82700000.

These sections exist but the 8280 install sections only reference `*_8280`, so they are unused. The SIDs match the IORT `LPASS_ADSP` and `NSP` entries above.

INFERRED: Qualcomm builds one driver source tree per SoC from shared templates, so SC7280 drivers ("7280"/"6490"-suffixed) will use the same architecture with their own ID range. The exact SC7280 `QCOMxxxx` IDs must come from the Q6A "6490" driver pack INFs (`Q6A_WoS_DriverPackage_251205_testsigned.7z`; forum posts name `qcpep6490.sys` and `QcXhciFilter6490`) or from the Q6A WP firmware DSDT.

## 6. External sources (web; read only, nothing downloaded)

* **Radxa Windows support for Q6A.**
  * Docs: <https://docs.radxa.com/en/dragon/q6a/other-system/windows>. Works out of the box: HDMI via GOP, PCIe/NVMe, eMMC, UFS, USB2/3. After the driver pack: GPU (D3D12, Vulkan 1.3, …), video codec, camera, GPIO. Not working: Wi-Fi and Bluetooth.
  * Driver pack: `Q6A_WoS_DriverPackage_251205_testsigned.7z`.
* **Firmware release notes** (<https://forum.radxa.com/t/radxa-dragon-q6a-firmware-snapshot/28886/20>):
  * `dragon-q6a_flat_build_wp_260120.zip`
  * the code base changed to **`qcm6490-wp-1-0_ap_standard_oem`**
  * works with both Windows and Linux
  * HDMI in UEFI, PXE/HTTP boot, GPIO setup, persistent UEFI variables
  * picks the DTB by kernel version
* **Forum thread** (<https://forum.radxa.com/t/windows-on-radxa-dragon-q6a/29913>, page 2):
  * UEFI 251215 and 251211/251214
  * `qcpep6490.sys` causes `SOC_CRITICAL_DEVICE_REMOVED (0x14E)` unless the test certificate is imported first
  * `QcXhciFilter6490` must be removed for USB / Windows To Go boot
  * I2C, UART and Shared Memory Port devices fail with "Object Path Component was not a directory object" / "Object Name not found" (DSDT path issues)
  * BitLocker, Secure Boot and TPM are disabled
  * tested on 25H2 build 26100
* **News coverage:** <https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/>.
* **Microsoft support list:** QCS6490/QCM6490 and Snapdragon 7c+ Gen 3 are supported for Windows 11 IoT Enterprise LTSC 2024, 24H2 and 25H2 (<https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/winiot_qualcomm_processors_2024>).
* **Open-source ACPI candidates:**
  * Project-Aloha `mu_aloha_platforms` has a prebuilt Kodiak DSDT for QRD7325: `Platforms/KodiakPkg/Device/qcom-qrd7325/ACPI/DSDT.aml` (<https://github.com/Project-Aloha/mu_aloha_platforms>). It also ships prebuilt Qualcomm DXEs, but no AcpiPlatform DXE.
  * WOA-Project `acpi_oem_qcom_common` has only 8150/8250/8350 tables, nothing for 7280 (<https://github.com/WOA-Project/acpi_oem_qcom_common>).

## 7. Implications for the RUBIK Pi 3 Windows plan

1. **ACPI table source, in order of fidelity:**
   * (a) Radxa's Q6A WP firmware (`dragon-q6a_flat_build_wp_2512xx/260120`). The user would download it; then run `fvparse.py` and look for `AcpiTableDxe`, the ACPI storage FFS and AML blobs. This is the same SoC with a production Qualcomm QCM6490 DSDT, IORT, PEP and CSRT.
   * (b) The Project-Aloha QRD7325 `DSDT.aml`.
   * (c) Hand-written tables, using this report's ID map and the IORT data from section 4.
2. Plan the ACPI/DT switch the way Qualcomm does it: a `DtAcpiPref`/ConfigTableMode-style variable, with ACPI installing `gEdkiiPlatformHasAcpiGuid` so the ACPI drivers load. Linux keeps the DT.
3. **EL level.**
   * Qualcomm's design (EnvDxe) keeps Gunyah for the ACPI/Windows choice. Our existing Gunyah-exit SMC (0x02000121/0x23) is the same call Qualcomm uses for the "DT w/ EL2" option.
   * Windows under Gunyah means no Hyper-V/VBS: the QcSkExt and Secure Kernel driver (`QCOM06AC`) would not apply (INFERRED).
   * The 8280 `qcsmmu` INF sets `HYPD Enabled=0`. This needs checking against the 6490 INF to learn whether Windows on 6490 expects to own the SMMU (EL2) or to go through Gunyah.
4. **Vendor DSDT objects the Qualcomm drivers need:**
   * DSDT `Name(QUFN)` (USB function stack choice)
   * a `_SUB` board ID (`QRD0…`-style) for the extension INFs
   * a Radxa-style vendor device is not required for us (`RAXA1001` is Radxa's own)
   * TrEE/secapp/SCM devices (`QCOM04DE`/`06E4`/`04DD`) need the TZ side: winsecapp and fTPM TAs, which are absent from the LE flat build
5. **Watch-outs from Radxa users:** the PEP driver bugchecks 0x14E without a valid configuration; the xHCI filter breaks USB boot; and wrong DSDT object paths break I2C/UART/GLINK. Phase the bring-up: a minimal DSDT with in-box drivers first (UFS, USB, PCIe, GOP), then the Qualcomm stack.
