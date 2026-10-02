# Prior art: Windows on SC7280 / SM7325 / QCS6490 ("kodiak") — status as of 2026-10-02

Scope: what other people have already done to boot Windows 11 ARM64 on the kodiak die, what can be
reused for the RUBIK Pi 3 port, and what is legal to use. Web pages were read only. No binaries
or driver packs were downloaded. The only things fetched were public ASL text dumps, small ACPI
.dat dumps, and CSV/MD metadata from GitHub.
Claims marked **INFERRED** are my own reasoning and are not stated by a source.

Reference copies (public text dumps, kept for analysis only and not to be committed) are in
`workspace/windows-research/prior-art-acpi/`:
- `aarch64-laptops_samsung-galaxybook2-go/`: full ACPI dump of a production SC7280 Windows laptop, as .dsl and .dat.
- `silicium-acpi/`: Silicium-ACPI Kodiak static tables, plus the lisa, a52sxq and spacewar DSDT .dsl files and the Mu-Silicium `AcpiTableUpdate.c`.
- `aloha-acpi7325/`: Project Aloha 7325 tables as .dsl.
- `woa-7280_WINDOWS_CLS/`: `BOM.csv` and `CHANGELOG.md` for the Qualcomm 7280 Windows driver set on Windows Update.
- `Qcom-Secure-Launch_README.md`.

---------------------------------------------------------------------------------------------------

## 0. Key findings (TL;DR)

1. **The same die already boots Windows 11 with a Qualcomm "WP" firmware: Radxa Dragon Q6A (QCS6490).**
   - The official preview started in Dec 2025. In Jan 2026 Radxa switched the Q6A to a single firmware for Windows and Linux: `dragon-q6a_flat_build_wp_260120`, whose base is Qualcomm `qcm6490-wp-1-0_ap_standard_oem` (closed-source Qualcomm UEFI with ACPI).
   - Radxa also ships a test-signed driver pack, `Q6A_WoS_DriverPackage_251205_testsigned.7z`, built from QCS6490.WP drivers (`*6490` names: `qcpep6490.sys`, `QcXhciFilter6490`, `qcdx6490`).
   - Without any drivers, these work: UFS, eMMC, NVMe/PCIe, USB 2/3 (devices must be plugged in before boot), and HDMI through the GOP framebuffer.
   - With the pack installed, these also work: GPU (D3D12 FL12_1, Vulkan 1.3), video, camera, GPIO/I2C/SPI and Ethernet. Wi-Fi/BT do not work.
2. **The local Q6A firmware (`flat_build_251013`) is NOT the Windows build.**
   - `contents.xml` says `QCM6490.LE.1.0` / `BOOT.MXF.1.0.c1-00364-KODIAKLA-1`.
   - Its `uefi.elf` FVs are gzip-compressed (guided section GUID `1D301FE9-BE79-4353-91C2-D23BC959AE0C`, at file offsets 0x4d1d0 and 0x218cf8). They contain `DtPlatformDxe` and no AcpiPlatform/DSDT. So no QCS6490 Windows ACPI tables are available locally.
3. **Public SC7280 Windows ACPI tables exist.** The canonical source is the aarch64-laptops dump of the Samsung Galaxy Book2 Go (7c+ Gen 3, SC7280).
   - Static tables: OEM `QCOM  `/`QCOMEDK2`, rev 0x7280.
   - DSDT: OEM `QCOMM `/`SDM7280 `, compiled by Microsoft `asl` 5.0.
   - The Kodiak tables in Project Silicium and Project Aloha are the same Qualcomm tables: field-for-field identical after normalisation, apart from patched addresses, the MADT layout and checksums. Aloha says explicitly that they were "Extracted from 7280 CLS firmware".
4. **The phone-port prior art runs Windows at EL1 under QHEE/Gunyah. It uses stock-firmware Qualcomm DXEs and Qualcomm's 7280 Windows ACPI.**
   - Ports: Mu-Silicium (Xiaomi 11 Lite 5G NE "lisa", Galaxy A52s "a52sxq") and Project Aloha (QRD7325, a52sxq, Tab S7 FE).
   - Windows drivers come from Qualcomm's 7c+ Gen 3 laptop driver set on Windows Update (`*7280.cab`). These are mirrored by WOA-Project, plus Radxa's QCM6490 GPU driver.
5. **Qualcomm's QCS6490 Windows BSP (QCS6490.WP, docs 80-86380-1) is behind a Qualcomm login.**
   - Microsoft lists QCS6490/QCM6490 as supported for Windows 11 24H2 (OEM list) and Windows 11 IoT Enterprise LTSC 2024/24H2/25H2.
   - Commercial Windows QCS6490 hardware: Advantech AOM-2721/AOM-DK2721 (Win11 IoT Ent preinstalled on UFS), Advantech MIO-5355, NexPhone (QCM6490).
6. **RUBIK Pi 3 has no Windows support.**
   - Thundercomm's Oct 2024 press release says "supports Qualcomm Linux, Android, and Windows". Its FAQ says Windows on ARM was "planned … in the course of 2025".
   - As of 2026-10-02 the docs list only Ubuntu, Android 13, Qualcomm Linux and Debian 13. The rubikpi-ai GitHub org has no Windows repository.
7. **Two facts from the public tables match what our port already does:**
   - Qualcomm's Windows MCFG for SC7280 is seg 0 = `0x60000000` and seg 1 = `0x40000000`, both buses 0–1. That is the DBI for bus 0 plus the 1 MiB iATU window at +1 MiB for bus 1 — exactly our PCIe0 layout.
   - The 7280 DSDT describes `usb_2` (0x08C00000) as `USB1` with `_HID QCOM0AA1` and `_CID PNP0D15`, so it binds the inbox xHCI driver.
   - Caveat: no public kodiak DSDT has a `PCI0` (PCIe0) device. We must write it ourselves (**INFERRED**: model it on `PCI1`).

---------------------------------------------------------------------------------------------------

## 1. Community ports (Project Silicium / Mu-Silicium, Project Aloha, Renegade edk2-msm, WOA-Project)

### 1.1 Devices on the kodiak die

| Project | Device (codename) | SoC | Windows status (per project) |
|---|---|---|---|
| Mu-Silicium | Xiaomi 11 Lite 5G NE (`lisa`) | SM7325 | UEFI: "Windows Boot ✅". OS: UFS, SD, USB host/device/PD, charging, WLAN, BT, GPS, touch, mobile data ✅; GPU ⚠️ (artifacts); speakers, mic, camera ❌ |
| Mu-Silicium | Samsung Galaxy A52s 5G (`a52sxq`) | SM7325 | UEFI: Windows Boot ✅. OS: UFS, SD, USB host/device, WLAN, BT, GPS, touch, sensors ✅; GPU, audio, camera ❌ |
| Mu-Silicium | Nothing Phone (1) (`spacewar`) | SM7325-AE | UEFI: Windows Boot ❌ (inactive) |
| Project Aloha | Qualcomm QRD 7325 (`qcom-qrd7325`) | SM7325 | DSDT ✅ (listed in README) |
| Project Aloha | `samsung-a52sxq`, `samsung-gts7fewifi` (Tab S7 FE Wi-Fi) | SM7325 | device folders present |
| Renegade edk2-msm | `Platform/Xiaomi/sm7325` (lisa, mona) | SM7325 | older; has `AcpiTables/` |
| Gezine/edk2-SMT733 | Galaxy Tab S7 FE Wi-Fi | SM7325 | separate EDK2 port |
| WOA-Project | `windows_silicon_qcom_kodiak` | — | placeholder: only `docs/`, created and last pushed 2024-12-14, README "everything" not working |

Sources:
- Mu-Silicium `Status.md`: <https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md>. The "Snapdragon 778G/778G+/782G Devices" section notes: "Only Windows Builds 26090 or higher Work". Our 25H2 ISO (26200) satisfies this.
- Aloha README: <https://github.com/Project-Aloha/mu_aloha_platforms> ("Snapdragon 778G/7c+ Gen 3 (SM7325/SC7280)" table).

### 1.2 How the ports work

- **Firmware base.** The ports use the Project Mu / EDK2 core (DxeMain, BDS, variables, FAT, console) and add **Qualcomm binary DXEs extracted from the device's stock (Android "LA") UEFI**. The binaries live in the `Project-Silicium/Device-Binaries` repo ("EFI Binaries from the Primary Bootloader of ARM64-Based Devices").
  - lisa's `Platforms/Xiaomi/lisaPkg/Include/DXE.inc` lists: EnvDxe, ShmBridgeDxeLA, ScmDxeLA, TzDxeLA, ChipInfo, PlatformInfo, DALSYS, HALIOMMU, HWIO, ClockDxe, SdccDxe, I2C, SPMI, TLMM, PmicDxeLa, UFSDxe, CmdDb, PwrUtils, Rpmh, Npa, ULog, Vcs, ICB, Smem, Gpi, SPI, FeatureEnabler, Buttons, ChargerEx, IPCC, GLink, PmicGlink, QcomChargerLA, UsbPwrCtrl, Tsens, Limits, DDRInfo, UsbfnDwc3, XhciPciEmulation, XhciDxe, UsbMsd, UsbDevice, UsbConfig, UsbInit, UCDxe, RngDxe, PILDxe and PILProxyDxe.
  - Display normally uses `SiliciumPkg/Drivers/SimpleFbDxe`, a framebuffer that XBL/ABL has already set up. Qualcomm's DisplayDxe is optional (`USE_CUSTOM_DISPLAY_DRIVER`).
  - The extraction tool is WOA-Project `UEFIReader` (<https://github.com/WOA-Project/UEFIReader>), which "generates .inf payloads … out of an existing UEFI volume".
  - **Contrast with us:** our port is open code (SmmuDxe, QcomUfsHcDxe, MdssDisplayDxe, GCC/TLMM libraries) and loads no Qualcomm DXEs.
- **SoC package.** `Silicon/Qualcomm/KodiakPkg/KodiakPkg.dsc.inc` sets:
  - `USE_PHYSICAL_TIMER = 0`: virtual timer, because UEFI runs at EL1 under QHEE.
  - errata flags `HAS_ACTLR_EL1_UNIMPLEMENTED_ERRATA=1` and `HAS_GIC_V3_WITHOUT_IRM_FLAG_SUPPORT_ERRATA=1`;
  - GICD at 0x17A00000 and GICR at 0x17A60000;
  - `PcdAcpiDefaultOemRevision 0x7325`.
- **ACPI.**
  - `Platforms/Xiaomi/lisaPkg/lisa.dsc` pulls `lisa/AcpiTables.inf` from the Silicium-ACPI submodule. That INF contains `DSDT.aml`, `Common/SSDT.aml` and `Kodiak/{APIC,CSRT,DBG2,FACP,GTDT,IORT,MCFG,PPTT,SPCR}.aml`.
  - `KodiakPkg/Library/AcpiTableUpdateLib/AcpiTableUpdate.c` patches DSDT `Name()` objects at boot (copy: `prior-art-acpi/silicium-acpi/Mu-Silicium_KodiakPkg_AcpiTableUpdate.c`):
    - SoC identity and SKU: `SOID`, `SIDV`, `SVMJ`/`SVMI`, `SDFE`, `SIDM`, `SIDS`, `SOSN`, `SOSI`, `PLST`, `SKUV`, `SDDR`;
    - storage: `STOR`, `SUFS`, `PUS3`, `SUS3`;
    - fTPM: `TPMA=1` and `TDTV=0x6654504D` ("fTPM");
    - modem/ADSP remote-FS carve-outs from the memory map: `RMTB`/`RMTX`, `RFMB`/`RFMS`/`RFAB`/`RFAS`;
    - `TCMA`/`TCML`, `PRP0`/`PRP1`, `UAON`;
    - and it forces `SOID = 0x237` (line 115).

  This is the list of DSDT knobs that Qualcomm's Windows drivers read (**INFERRED** from the name list; the PEP and PCIe `_STA` use `PRPx` — e.g. `PCI1._STA` returns 0x0F only if `PRP1 == One`).
- **QHEE / EL.** On these phones UEFI and Windows both run at EL1 under QHEE/Gunyah. The 7280 FADT carries `Hypervisor Vendor Identity = 0x4D4F4351` ("QCOM"): `prior-art-acpi/aarch64-laptops_samsung-galaxybook2-go/FACP.dsl:176`.
  - Mu-Silicium v3.2 (2026-02-01) added an "SMMU Detach" library (`Silicon/Qualcomm/QcomPkg/Library/ArmSmmuDetachLib/ArmSmmuDetachLib.c`, MIT, derived from DuoWoA/Aloha). It is enabled for Kodiak.
  - Before handing off, the library walks the apps-SMMU SMR/S2CR entries. Every stream not on a keep-list is set to FAULT and its SMR is invalidated, and its context bank is reset (TTBRs/TCR cleared, CBAR set to S1-translate/S2-bypass with VMID 0xFF). Bootloader-era mappings therefore do not survive into Windows. Release notes: <https://github.com/Project-Silicium/Mu-Silicium/releases>.
  - None of the phone ports offer Hyper-V/VBS (**INFERRED**). On Qualcomm WoA devices EL2 is only reachable through "Secure Launch" (§3.3).
- **Secure Boot / TPM.**
  - Mu-Silicium "Implemented Secure Boot" in v3.1 (2026-01-01) and ships a `SiPolicy.p7b` with community certificates (`SiliciumPkg/Include/Resources/SecureBoot/README.md`).
  - Aloha `KodiakPkg/Kodiak.dsc` builds with `SECURE_BOOT = 1`, Microsoft KEK/DB 2011 and 2023 certificates and the default DBX. There is also a `KodiakNoSb.dsc` variant.
  - Aloha also installs the Qualcomm `TPM2.aml` (Start Method 9 = Qualcomm TrEE/fTPM) and `FACS.aml`.
  - Neither project documents a working fTPM on kodiak phones (**INFERRED**: Windows setup needs the usual TPM/SB check bypass, e.g. Rufus).

### 1.3 Windows driver sources used by the ports

- **WOA-Project/Qualcomm-Reference-Drivers** (<https://github.com/WOA-Project/Qualcomm-Reference-Drivers>) mirrors Windows Update packages for the "Qualcomm Reference Clamshell (CLS) Laptop with SC7280". The folders are `7280_CLS` (pre-release, 200.0.1–200.0.4) and `7280_WINDOWS_CLS` (200.0.1.0 dated 3/10/2022 through **200.0.15.0 dated 7/8/2024**).
  - 200.0.15.0 contains about 110 `.cab` files at driver version 1.0.4056.4400 / 30.0.4056.4400 (GPU), plus `qcfirmware7280_{UFS,EMMC,NVME}.cab` v1.0.6600.0. Those firmware cabs are **UEFI capsules** with `UEFI\RES_{cc390084-098a-44cd-abdd-10bab3fa8341}` for UFS. The Aloha ACPI tables were extracted from them.
  - Hardware IDs from `prior-art-acpi/woa-7280_WINDOWS_CLS/BOM.csv` (BOM 200.0.13.0):

| Package | Hardware ID(s) | Device / note |
|---|---|---|
| `qcpep.wd7280` | `ACPI\QCOM0A17` | PEP — nearly every device has `_DEP` on it |
| `qcsmmu7280` | `ACPI\QCOM0A09` | SMMU |
| `qciommu` | `ACPI\QCOM068F` | IOMMU |
| `qcgpio7280` | `ACPI\QCOM0A0C` | GPIO |
| `qci2c7280` | `QCOM0A10` | I2C |
| `qcuart7280` | `QCOM0A16` | UART |
| `qcspmi7280` | `QCOM0A0B` | SPMI |
| `qcpmic7280` | `QCOM0A2B` | PMIC |
| `qcppx7280` | `ACPI\QCOM0A96` | "PCIe Platform Extension Plugin"; `PCI1` has `_DEP` on PEP0 + QPPX |
| `QcXhciFilter7280` | `ACPI\QCOM0A24`, `ACPI\QCOM0AA1`, `URS\QCOM0A8B&HOST` | USB host filter |
| `QcUsbFnSsFilter7280` | — | USB device-mode filter |
| `qcusbcucsi7280` | `QCOM0AA4` | USB-C UCSI |
| `qcsubsys7280` | ADSP `QCOM0A1B`, MPSS `0A1C`, CDSP `0AB0`, WPSS `0AE2` | DSP subsystems |
| `qcpil` | `QCOM06E0` | peripheral image loader |
| `QcTrEE` | `QCOM04DE` | TrEE / fTPM path |
| `qcscm` | `QCOM04DD` | SCM |
| `qcsecapp` | `QCOM0AE3` | secure app |
| `qcdx7280` | `ACPI\VEN_QCOM&DEV_0A36&REV_xxxx` | Adreno "7c+ Gen 3" |
| `qcwlan7280` | `WPSS\VEN_QCOM&DEV_0A28` | WCN6750 |
| `qcaudminiport*`, `qcadcm`, `qcaucd` | — | audio |

  - Board-extension INFs bind on `_SUB` values `CRD07280`, `IDP07280`, `IDPS7280`, `QRD07280`, `MTP07280`, `CLS07280`, ….
- **AistopGit/windows_oem_xiaomi_lisa** (<https://github.com/AistopGit/windows_oem_xiaomi_lisa>): its README says the drivers "are taken from Qualcomm-Reference-Drivers" and that it contains "binary files sourced from Qualcomm Snapdragon 7c+ Gen 3 laptops/tablets as well as the Surface Duo original android firmware".
  - Commit `1961d909dd` (2025-12-20) added GPU support from "the QCM6490 GPU driver published by Radxa Team": `components/QC7325/Graphics/GRAPHICS.SOC_QC7325.LISA_DESKTOP/qcdx6490.inf`, `DriverVer = 10/02/2025,30.0.4444.9300`.
  - That INF binds **`ACPI\VEN_QCOM&DEV_0E36`**, not 0A36. The lisa DSDT therefore names the GPU `QCOM0E36` (`silicium-acpi/Platforms/Xiaomi/lisa/Decompiled/DSDT.dsl:15915`). **Consequence:** the QCS6490.WP driver set uses at least some different HIDs from the 7280 laptop set, so the DSDT HIDs must match whichever driver set is chosen.
- Others:
  - woa-a52s/windows_oem_samsung_a52sxq;
  - Icesito68/7xx-Drivers (QC7325 components, last push 2024-03);
  - edk2-porting/WOA-Drivers (SDM845-era).
- No OEM driver pack exists for the Galaxy Book2 Go. aarch64-laptops/build issue #138 (2025-05-23) asks for one: <https://github.com/aarch64-laptops/build/issues/138>.

---------------------------------------------------------------------------------------------------

## 2. Qualcomm / Microsoft: Windows 11 (IoT Enterprise) on QCS6490

- **Microsoft "Supported Qualcomm Processors for Windows 11 IoT Enterprise (2024 to present)":** QCS6490/QCM6490 (and QCS5430/QCM5430) for "Windows 11 IoT Enterprise LTSC 2024, version 24H2, version 25H2". 7c+ Gen 3 is also listed. Page dated 2026-06-24: <https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/win11_ltsc_2024_qualcomm_processors>
- **Microsoft "Windows 11, version 24H2 supported Qualcomm processors" (OEM list):** includes QCM6490, QCS6490, QCM5430, QCS5430 and Snapdragon 7c+ Gen 3: <https://learn.microsoft.com/en-us/windows-hardware/design/minimum/supported/windows-11-24h2-supported-qualcomm-processors>
- **Qualcomm "QCS6490 Windows resources"** (doc 80-86380-1, Rev AF, updated 2025-05-12; search-snippet data): <https://docs.qualcomm.com/doc/80-86380-1/topic/qcs6490-windows-software-resources.html>
  - The landing page is public and names the software product **`QCS6490.WP`**. Linked topics: QCS6490.WP Release Notes, Software Architecture, Embedded Controller, INFs and QTI Drivers, Windows Drivers, Factory Manufacturing, UEFI, Clock Plan, ACPI, Peripherals, PCIe, Partitioning Layout, Storage, Subsystem Restart, Power Management, Thermal, Security, Secure Boot Enablement, and "Provisioning for Windows Devices / Platform Bringup".
  - Following "All Release Notes" redirects to `account.qualcomm.com` sign-in, so the content is gated (checked 2026-10-02).
  - The software itself is distributed only to licensed Qualcomm customers (**INFERRED** from the login gating and the usual Qualcomm ChipCode practice).
- **Hardware shipping Windows on QCS6490/QCM6490:**
  - Advantech AOM-DK2721 / AOM-2721 OSM: "128GB UFS built-in MS Windows 11 IoT Enterprise". <https://www.advantech.com/en-us/products/risc_evaluation_kit/aom-dk2721/mod_0e561ece-295c-4039-a545-68f8ded469a8>. How-to: <https://forum.aim-linux.advantech.com/t/how-to-article-get-started-with-windows-11-iot-enterprise-on-qualcomm-dragonwing-qcs6490-using-the-aom-2721-development-kit/1165>
  - Advantech MIO-5355 (3.5" SBC, "Windows on Arm"; LinuxGizmos, 2026-01-21).
  - Quectel QSM560DR ("Windows … under development", CNX 2025-11-18).
  - NexPhone (QCM6490; Android, Debian and Windows 11 multi-boot; shipping Q3 2026; <https://nexphone.com/>).
  - Qualcomm/Thundercomm RB3 Gen 2: the Qualcomm product brief and the Microsoft list say QCS6490 supports Win11 IoT Enterprise, but the Thundercomm RB3 Gen 2 product page lists only Linux-based OSes (<https://www.thundercomm.com/product/qualcomm-rb3-gen-2/>). No public RB3 Gen 2 Windows image was found.
- **Licensing, realistically:**
  - Windows 11 IoT Enterprise is sold through Microsoft OEM/IoT distributors.
  - The QCS6490.WP BSP (UEFI with ACPI, signed drivers, TZ apps) comes from Qualcomm under NDA/licence.
  - For hobby use the practical route is the retail Windows 11 ARM64 ISO plus an activation licence, which is what Radxa recommends.

---------------------------------------------------------------------------------------------------

## 3. Radxa Dragon Q6A (QCS6490) — the closest prior art

### 3.1 Status and timeline
- Forum thread "Windows on Radxa Dragon Q6A", started by strongtz (Radxa): <https://forum.radxa.com/t/windows-on-radxa-dragon-q6a/29913>
  - UEFI 251211, then 251214 (fixes KERNEL_SECURITY_CHECK_FAILURE), then 251215 (fixes RTC "300 years" and UFS detection). All Dec 2025.
  - Driver pack `Q6A_WoS_DriverPackage_251205_testsigned.7z` and guide "Windows on Radxa Dragon Q6A - 251213.pdf", both on node0.momosan.cc.
  - A user quotes "qcom's UEFI is closed sourced".
- Firmware snapshot thread (<https://forum.radxa.com/t/radxa-dragon-q6a-firmware-snapshot/28886>): release **2026-01-20** "Changed base Qualcomm code" to **`qcm6490-wp-1-0_ap_standard_oem`**, "compatible with both Windows and Linux". The same release adds HDMI in UEFI, PXE/HTTP boot, GPIO setup UI, persistent variables, and automatic DTB selection by kernel version.
- Download page (<https://docs.radxa.com/en/dragon/q6a/download>): `https://dl.radxa.com/dragon/q6a/images/dragon-q6a_flat_build_wp_260120.zip`. Not downloaded, per the rules.
- Official install doc (<https://docs.radxa.com/en/dragon/q6a/other-system/windows>):
  - Windows 11 24H2 or later, retail ISO or UUP dump, written with **Rufus** to GPT/UEFI;
  - F12 boot menu; targets NVMe, UFS or eMMC;
  - then `1-testsigning.bat`, import `radxa_wos_test_cert.reg`, and run `2-DriverSetInst.bat`. Test signing is mandatory.
- CNX, 2025-12-18 (<https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/>):
  - **Works without drivers:** "HDMI output (inherited from UEFI GOP), PCIe (NVMe), eMMC, UFS, USB 2.0, USB 3.0 (with devices connected before boot)".
  - **With drivers:** D3D11VA/DXVA2 decode up to 4K60, MF encode 4K30, D3D12 FL12_1, OpenCL 3.0, Vulkan 1.3, OpenGL 4.1, CSI camera (Spectra 570L), 40-pin GPIO, Ethernet RTL8111K.
  - **Not working:** Wi-Fi, BT. The community fallback is AIC8800 `aicwlan_arm64_0935761.zip` from worproject/dldserv-mirror.
- Issues reported in the thread:
  - `qcpep6490.sys` is critical. If it is missing or the certificate import is broken, the result is `SOC_CRITICAL_DEVICE_REMOVED (0x14E)`, because "Almost all system components depend on the PEP driver".
  - `QcXhciFilter6490` breaks booting from USB enclosures and must be removed for USB boot.
  - Ethernet drops under load; Radxa later posted a Realtek `PCIE_Win10_10074_arm64…` driver.
  - Several Qualcomm devices fail with "Object Name not found", which means DSDT/driver mismatches on some installs.
  - Successfully installed devices include "Qualcomm ACPI Bridge, BAM Bus, IOMMU, System Manager, System MMU, Radxa WoS Platform Device".
- SBCwiki notes "Drivers supplied from vendor … not Qualcomm" and "Boots regular ARM ISO's from Microsoft": <https://sbcwiki.com/docs/soc-manufacturers/qualcomm/dragonwing-6490/boards/radxa-dragon-q6a/>

### 3.2 What the Q6A firmware does with Gunyah / EL2
- From TravMurav "Qcom-Secure-Launch" (<https://github.com/TravMurav/Qcom-Secure-Launch>; copy at `prior-art-acpi/Qcom-Secure-Launch_README.md:59-76`): the Q6A "seems to run WoA firmware stack but includes some additions that allow the EFI firmware to take over Gunyah".
  - `EnvDxe` checks `/chosen/radxa,enable-kvm`. If it is present, EnvDxe issues `smc(0x2000121, 0, 0, 1)` at ExitBootServices, and Gunyah returns to the caller in EL2.
  - The same call with the last argument 0 tells Gunyah to keep running the OS at EL1.
  - This is the same mechanism our port uses (UEFI can stay at EL1 or remove Gunyah).
- Windows on QCS6490 by default boots at EL1 under Gunyah, with FADT hypervisor vendor "QCOM" (**INFERRED** from the 7280 FADT).
- Hyper-V/VBS on Qualcomm WoA uses **Secure Launch**: `winload` → `tcblaunch.exe` → SMCs verified by the `mssecapp` TZ app against Microsoft keys, after which `tcblaunch` owns EL2. That needs the WP TrustZone apps. Our LE/QLI firmware very likely lacks them (**INFERRED**). Strings in the RUBIK Pi 3 stock `uefi.elf` and the Q6A 251013 LE `uefi.elf` show only `qcom.tz.uefisecapp` / `storsecapp`, plus a comment "AppPartitionId = 5 reserved for WinsecApp Ftm"; no mssecapp/winsecapp loader strings were found.

### 3.3 Locally available Radxa material
- `<downloads>/dragon-q6a_flat_build_251013/…/contents.xml`: `QCM6490.LE.1.0-00376-STD.PROD-1`, `BOOT.MXF.1.0.c1-00364-KODIAKLA-1`, `TZ.XF.5.29.1-00084`, `TZ.APPS.1.29-00208`. This is the Linux build.
- The two gzip FVs of its `uefi.elf` decompress to 5.6 MB and 4.9 MB. They list `DtPlatformDxe` (with `DtAcpiPref`, `gEdkiiPlatformHasAcpiGuid`), `SmBiosTableDxe`, `UsbConfigDxe` (with ACPI-update code paths) and `PILDxe`. They contain no ACPI table images.
- `<downloads>/dragon-q8b_win_driver_pack_v1.0.0/driver_pack/` holds 72 folders of SC8280XP `*8280` drivers (pack v1.0.0) and a Radxa `radxaplatform.inf` (`ACPI\RAXA1001`, `ACPI\RAXA1002`, DriverVer 08/24/2026). It is useful as a model of the WoS pack layout and the Radxa platform device. The Q6A (6490) pack itself is not local.

---------------------------------------------------------------------------------------------------

## 4. Thundercomm RUBIK Pi 3

- PR Newswire, 2024-10-08: "RUBIK Pi supports Qualcomm® Linux®, Android, and Windows". <https://www.prnewswire.com/news-releases/thundercomm-launches-rubik-pi-on-qualcomm-platforms-302269863.html>
- Atlantik Elektronik FAQ: "planned (Ubuntu, Windows on ARM) in the course of 2025". <https://www.atlantikelektronik.com/en/qualcomm-technologies/rubik-pi/rubik-pi-faq>
- Current docs (<https://www.thundercomm.com/rubik-pi-3/en/docs/about-rubikpi/>): "OS support: Canonical Ubuntu for Qualcomm platforms, Android 13, Qualcomm® Linux®, Debian 13". No Windows.
- GitHub org `rubikpi-ai`: no Windows repository.
  - `rubikpi-ai/boot-assets` (branch `qli2.0`, last push 2026-09-15) is the QLI 2.0 boot chain, including `xbl_config_gunyah.elf` and `xbl_config_kvm.elf` variants.
  - Its `LICENSE.txt` is the standard Qualcomm licence: redistribution of binaries only together with Qualcomm chipsets, the licence text must be included, and §1.3 forbids reverse engineering and decompiling.
- **Peripheral gap vs. Qualcomm Windows drivers** (from `devicetree/mainline/upstream/src/arm64/qcom/qcs6490-thundercomm-rubikpi3.dts`):
  - Wi-Fi/BT is an AP6256 (Broadcom BCM43456) on SDIO `sdhc_2` plus UART (`brcm,bcm4345c5`, dts:997-1083). The WCN6750/WPSS path is deleted (dts:31-33), so neither the Qualcomm `qcwlan` nor the Radxa AIC driver applies. No Windows ARM64 driver is known (**INFERRED**: Wi-Fi/BT will not work).
  - HDMI is DSI → LT9611. The Qualcomm `qcdx` display path needs a DSI panel/bridge description; the CRD/IDP extensions target eDP/DSI panels, not the LT9611 (**INFERRED**: the GOP/BasicDisplay framebuffer is the realistic first step).
  - USB 3 and Ethernet sit behind the Renesas uPD720201 (inbox `usbxhci`) and the AX88179. The AX88179 needs the ASIX Windows ARM64 driver, which may come through Windows Update (**INFERRED**).

---------------------------------------------------------------------------------------------------

## 5. Public SC7280 / SM7325 ACPI tables

| Source | Path / URL | What | Provenance |
|---|---|---|---|
| aarch64-laptops/build | `misc/samsung-galaxybook2-go/` — <https://github.com/aarch64-laptops/build/tree/master/misc/samsung-galaxybook2-go> (commit 98b3c0f023, 2024-02-24) | APIC, BGRT, CSRT, DBG2, DSDT (91330 B), FACP, FPDT, GTDT, IORT, MCFG, PPTT, SPCR, TPM2, XSDT as `.dat` + `.dsl` | dumped from a retail SC7280 laptop. Static tables `QCOM/QCOMEDK2` rev 0x7280, compiler `QCOM`; DSDT `QCOMM/SDM7280` compiled by MSFT 5.0 |
| Project-Silicium/Silicium-ACPI | `Silicon/Qualcomm/Kodiak/*` + `Platforms/{Xiaomi/lisa,Samsung/a52sxq,Nothing/spacewar}/` — <https://github.com/Project-Silicium/Silicium-ACPI> | static Kodiak tables (re-assembled by iasl 20230628, rev 0x7325) plus per-device DSDTs: lisa 114451 B, a52sxq 83917 B (both `SDM7280` / MSFT), spacewar 5198 B (minimal) and `DSDT_Minimal.asl` (CPUs only) | static tables are field-identical to the Galaxy Book2 Go dump. My diff shows only DSDT/FACS addresses, MADT GICC length (0x50→0x52) and checksum bytes differ. DSDTs are Qualcomm 7280 DSDTs edited per device (e.g. commit 9de5e4d4f9 "SM7325: add other ACPI tables", 2024-12-10). No licence file |
| Project-Aloha/acpi_oem_third_party_common | `7325/{builtin,src}` — <https://github.com/Project-Aloha/acpi_oem_third_party_common> | BGRT, CSRT, DBG2, FACP, FACS, GTDT, IORT, MADT, MCFG, PPTT, SPCR, TPM2, dsdt (85861 B, `SDM7280`) | commit f78227c740 (2023-05-03): "Add 7325 ACPI Tables. Extracted from 7280 CLS firmware" (the Windows Update capsule). readme: "ACPI tables for SM7325 are based on SC7280 tables". No licence |
| edk2-porting/edk2-msm | `Platform/Xiaomi/sm7325/AcpiTables`, `Silicon/Qualcomm/sm7325/AcpiTables` — <https://github.com/edk2-porting/edk2-msm> | older lisa tables | community |
| Radxa Q6A WP firmware | inside `dragon-q6a_flat_build_wp_260120.zip` | the only QCS6490 (WP) ACPI set | **not public as text**. It would have to be extracted from the Qualcomm-licensed binary; the Qualcomm licence forbids reverse engineering |

### 5.1 Facts from the SC7280 Windows tables that matter for RUBIK Pi 3
All from `prior-art-acpi/aarch64-laptops_samsung-galaxybook2-go/`.

- **FADT** (`FACP.dsl`): rev 5; Hardware-Reduced=1; Low-Power-S0-Idle=1; PSCI compliant via SMC (not HVC); PM profile 8 (Tablet); Hypervisor Vendor ID "QCOM" (line 176). FACS at 0xFFF21000.
- **MADT** (`APIC.dsl`): 8 GICC entries; GICD 0x17A00000 (line 215); GICR range 0x17A60000, length 0x100000 (line 223); a GIC MSI frame at 0x17A10000 (line 230).
- **GTDT** (iasl prints the values in hex):

| Timer | Interrupt |
|---|---|
| Secure EL1 | 0x1D (29) |
| Non-secure EL1 | 0x1E (30) |
| Virtual | 0x1B (27) |
| Non-secure EL2 | 0x1A (26) |

  CntControlBase and CntReadBase are both 0xFFFF…FFFF. A platform timer block (timer interrupt 0x28, virtual 0x26) is present.
- **MCFG** (`MCFG.dsl:25-32`): seg 0 at 0x60000000 and seg 1 at 0x40000000, both buses 0–1. This is the DBI as bus 0 plus the 1 MiB iATU CFG window at +0x100000 as bus 1 — identical to our PCIe0 implementation.
- **DSDT `PCI1`** (`DSDT.dsl:15356`): `_SEG 1`, `_CCA 1`, `_DEP {PEP0, QPPX}`, `_STA` gated on `PRP1`, `_CRS` Memory32Fixed 0x40300000 length 0x1D00000 plus bus 0–1, `_PRT` INTA–D = GSI 0x1D2, 0x1D3, 0x1D6, 0x1D7, and an `_OSC`.
  - **There is no `PCI0`** in any public kodiak DSDT: Galaxy Book2 Go, Aloha 7325, Silicium lisa and a52sxq.
  - **INFERRED:** we must write PCI0 by analogy: `_SEG 0`, MMIO from the Linux DT `pcie0` ranges, `_PRT` from the DT `interrupt-map`, and either drop `_DEP` on PEP/QPPX or supply the Qualcomm drivers.
- **USB:**
  - `USB1` (`DSDT.dsl:20824`): `_HID QCOM0AA1`, `_CID ACPI\PNP0D15`, MMIO 0x08C00000/0xFFFFF, interrupts 0x112, 0x111, 0x20D, 0x20C. This is `usb_2`, our USB 2.0 Type-A port.
  - `USB0`: `QCOM0A24` / `PNP0D15` at 0x0A600000.
  - `URS0`: `QCOM0A8B` / `PNP0CA1` (dual-role usb_1).
  - `UBTC`: `USBC000` / `PNP0CA0` (UCSI).
- **UFS0** (`DSDT.dsl:243`): `QCOM24A5`, MMIO 0x01D84000 length 0x1C000, interrupt 0x129. **INFERRED:** binds the inbox UFS driver, consistent with "UFS works without drivers" on Q6A.
- **IORT:** apps SMMU 0x15000000 (model 3 = MMU-500, line 34); GPU SMMU 0x3DA0000 (line 149). Root complexes for seg 0 and seg 1. Named components `GPU0, JPGE, ARPC, IPA, USBA, QDSS, ADSP.ADCM, SDC1, SDC2, UFS0, URS0, URS1, GPU0.AVS0, WPSS.QWLN`.
- **SPCR:** interface type 0x11 (Qualcomm GENI UART) at 0x00A90000, GSI 0x182. **DBG2:** UART plus USB-debug (0x0A600000) entries.
- **TPM2:** rev 3, Start Method 9, control area 0x808A4000 (TrEE fTPM — needs Qualcomm TZ apps plus the `QcTrEE` driver).
- The DSDT contains `PEP0` (`QCOM0A17`, line 1192) with a ~11k-line PEP resource package. That is the bulk of the Qualcomm-specific DSDT and is only useful together with `qcpep`.

---------------------------------------------------------------------------------------------------

## 6. What is realistically obtainable and legal to use

| Item | Obtainable? | Legal / licence status | Use in this project |
|---|---|---|---|
| Windows 11 ARM64 ISO (25H2, already local) | yes | needs a Windows licence to activate; installing is allowed | install media |
| Windows 11 IoT Enterprise | only via OEM/distributor | commercial licence | not needed |
| Qualcomm QCS6490.WP BSP (UEFI, ACPI, drivers, docs) | no (Qualcomm login/licence) | NDA | not available |
| Radxa Q6A WP firmware (`flat_build_wp_260120.zip`) | public download (not fetched) | Qualcomm binary licence (redistribution only with Qualcomm chips; reverse engineering prohibited, per the licence text in rubikpi-ai/boot-assets) | do not extract ACPI from it without accepting that licence risk; user decision |
| Radxa Q6A driver pack (test-signed) | public download (not fetched) | no explicit licence; test-signed; meant for Q6A | possible personal-use source of `*6490` drivers. Needs test signing and a DSDT that matches its HIDs (e.g. `QCOM0E36` GPU) |
| WOA-Project mirror of 7280 Windows Update drivers | public | Qualcomm-signed WHQL packages; the mirror disclaims affiliation; redistribution rights unclear | personal-use install only. Never commit binaries. HIDs/INFs serve as reference for DSDT naming |
| SC7280 ACPI dumps (aarch64-laptops, Silicium, Aloha) | public text | Qualcomm/Samsung firmware content; no licence granted (**INFERRED**: copyrighted, use as reference only) | write our own ASL from the Linux DT and specs. Use the dumps only to learn Windows HIDs, `_CID`, MCFG layout and field conventions. Do not copy the PEP0 package wholesale into the repo |
| Mu-Silicium code (BSD-2-Clause-Patent; GPL in GPL* dirs; ArmSmmuDetachLib MIT) | public | open source | ArmSmmuDetachLib is a model for clean SMMU hand-off; the AcpiTableUpdate name list documents the DSDT knobs |
| Linux kernel DTs / drivers | public (GPL-2.0 / dual BSD for DTs) | open | primary source for addresses, IRQs and regulators in our own ASL |

---------------------------------------------------------------------------------------------------

## 7. Implications for the RUBIK Pi 3 plan (summary, all INFERRED)

1. **Stage 1, boot and install.** A small set of open, self-written tables should be enough for Windows to install from USB to UFS and boot, with inbox drivers only:
   - FADT, MADT, GTDT, DSDT, MCFG and SPCR/DBG2, plus PPTT and IORT if needed;
   - DSDT with CPUs, `UFS0 QCOM24A5`, `USB1 QCOM0AA1`/`PNP0D15` for usb_2, and a self-authored `PCI0` (seg 0, MCFG 0x60000000) for the Renesas xHCI;
   - no `_DEP` on PEP; GOP framebuffer for display.

   The Q6A preview shows the same die doing exactly this ("works without drivers: HDMI-GOP, UFS, PCIe, USB2/3").
2. **Stage 2, Qualcomm drivers.** This needs a DSDT shaped like Qualcomm's (`PEP0` + PEP packages, `_SUB`, `PRPx`/`SOID` names) and one consistent driver set:
   - (a) the 7280 Windows Update set (HIDs `QCOM0Axx`, `GPU 0A36`), or
   - (b) Radxa's 6490 set (GPU `0E36`; `qcpep6490`).

   Mixing the two sets is risky. A wrong PEP leads to bugcheck 0x14E.
3. **EL choice.** Windows at EL1 under Gunyah (FADT hypervisor "QCOM") is what every Qualcomm Windows device does. Booting Windows at EL2 after removing Gunyah is untested in the prior art. Hyper-V on Qualcomm uses Secure Launch (`mssecapp`), which our LE TrustZone probably lacks.
4. **Not obtainable or unlikely:** fTPM (no winsecapp TA), Wi-Fi/BT (Broadcom SDIO), LT9611 HDMI via `qcdx`, audio. Plan for TPM/Secure Boot setup bypass, USB Wi-Fi or Ethernet, and the BasicDisplay framebuffer.

---------------------------------------------------------------------------------------------------

## Sources (all read 2026-10-02)

- Project Silicium:
  - <https://github.com/Project-Silicium/Mu-Silicium> (`Status.md`, `Silicon/Qualcomm/KodiakPkg/*`, `Platforms/Xiaomi/lisaPkg/*`, `Silicon/Qualcomm/QcomPkg/Library/ArmSmmuDetachLib/*`, releases)
  - <https://github.com/Project-Silicium/Silicium-ACPI>
  - <https://github.com/Project-Silicium/Device-Binaries>
- Project Aloha:
  - <https://github.com/Project-Aloha/mu_aloha_platforms>
  - <https://github.com/Project-Aloha/acpi_oem_third_party_common>
- WOA-Project:
  - <https://github.com/WOA-Project/Qualcomm-Reference-Drivers> (`7280_WINDOWS_CLS/BOM.csv`, `CHANGELOG.md`)
  - <https://github.com/WOA-Project/windows_silicon_qcom_kodiak>
  - <https://github.com/WOA-Project/acpi_oem_qcom_common>
  - <https://github.com/WOA-Project/UEFIReader>
- Kodiak driver repos:
  - <https://github.com/AistopGit/windows_oem_xiaomi_lisa> (commit 1961d909dd)
  - <https://github.com/woa-a52s/windows_oem_samsung_a52sxq>
  - <https://github.com/Icesito68/7xx-Drivers>
- Other UEFI ports: <https://github.com/edk2-porting/edk2-msm>, <https://github.com/Gezine/edk2-SMT733>
- aarch64-laptops: <https://github.com/aarch64-laptops/build/tree/master/misc/samsung-galaxybook2-go>, issue #138
- EL2 / Secure Launch: <https://github.com/TravMurav/Qcom-Secure-Launch>
- Radxa:
  - <https://docs.radxa.com/en/dragon/q6a/other-system/windows>
  - <https://docs.radxa.com/en/dragon/q6a/download>
  - <https://forum.radxa.com/t/windows-on-radxa-dragon-q6a/29913> (pages 1–3)
  - <https://forum.radxa.com/t/radxa-dragon-q6a-firmware-snapshot/28886>
  - <https://www.cnx-software.com/2025/12/18/radxa-dragon-q6a-arm-sbc-get-official-windows-11-preview/>
  - <https://sbcwiki.com/docs/soc-manufacturers/qualcomm/dragonwing-6490/boards/radxa-dragon-q6a/>
  - <https://github.com/strongtz/radxa-wos-gpio-demo>
- Microsoft:
  - <https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/hardware/supported/win11_ltsc_2024_qualcomm_processors>
  - <https://learn.microsoft.com/en-us/windows-hardware/design/minimum/supported/windows-11-24h2-supported-qualcomm-processors>
- Qualcomm: <https://docs.qualcomm.com/doc/80-86380-1/topic/qcs6490-windows-software-resources.html> (landing page public; content behind login)
- Advantech:
  - <https://www.advantech.com/en-us/products/risc_evaluation_kit/aom-dk2721/mod_0e561ece-295c-4039-a545-68f8ded469a8>
  - <https://forum.aim-linux.advantech.com/t/how-to-article-get-started-with-windows-11-iot-enterprise-on-qualcomm-dragonwing-qcs6490-using-the-aom-2721-development-kit/1165>
  - <https://linuxgizmos.com/qualcomm-qcs6490-based-3-5sbc-supports-yocto-ubuntu-and-windows-on-arm/>
- Other QCS6490/QCM6490 hardware:
  - <https://www.cnx-software.com/2025/11/18/quectel-qsm560dr-industrial-qualcomm-qcm6490-qcs6490-sbc-supports-ubuntu-android-and-windows/>
  - <https://www.gsmarena.com/nexphone_is_a_desktop_replacement_smartphone_that_multiboots_android_linux_and_windows_11__-news-71205.php>
- Thundercomm:
  - <https://www.thundercomm.com/product/rubik-pi/>
  - <https://www.thundercomm.com/rubik-pi-3/en/docs/about-rubikpi/>
  - <https://www.atlantikelektronik.com/en/qualcomm-technologies/rubik-pi/rubik-pi-faq>
  - <https://www.prnewswire.com/news-releases/thundercomm-launches-rubik-pi-on-qualcomm-platforms-302269863.html>
  - <https://github.com/rubikpi-ai/boot-assets>
  - <https://www.thundercomm.com/product/qualcomm-rb3-gen-2/>
- Local files:
  - `<downloads>/dragon-q6a_flat_build_251013/flat_build/spinor/dragon-q6a/{contents.xml,uefi.elf}`
  - `<downloads>/dragon-q8b_win_driver_pack_v1.0.0/driver_pack/`
  - `workspace/rubikpi3-backup/stock-uefi.elf`
  - `devicetree/mainline/upstream/src/arm64/qcom/qcs6490-thundercomm-rubikpi3.dts`
