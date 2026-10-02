# What Windows 11 ARM64 boot manager and OS loader need from the RUBIK Pi 3 firmware

Task: find out, from the code, what `bootaa64.efi` (Windows Boot Manager) and `winload.efi` (the OS loader) of the
Win11 25H2 ARM64 ISO need from UEFI firmware, and from that define the **minimum ACPI/UEFI set that gets the
installer kernel running**. The kernel's built-in ARM64 HAL (`ntoskrnl.exe`) is covered in a short section too,
because "kernel running" depends on it.

No public symbols were used. Function names come from winload's and ntoskrnl's **export tables** (winload exports
477 `Bl*`/`Osl*`/`Hvl*` routines for `hvloader.dll`). Everything else is read from the code. Addresses are virtual
addresses at the PE image base (bootmgr 0x10000000, winload 0x180000000, ntoskrnl 0x140000000). They are stable, unlike
line numbers in the generated `.asm` files. Anything not directly read from code is marked **INFERRED**.

## 0. TL;DR

* **Fatal ACPI lookups in the boot apps: only two.**
  * `FACP` is fatal in both library initialisations: bootmgr `0x101af6cc` → error path `0x101b0428`, and winload
    `0x18018d71c` → `0x18018e914`.
  * `APIC` (MADT) is fatal in winload when it selects the kernel and HAL: `0x18001381c` in `OslpLoadAllModules`, failure
    returned at `0x180012b4c`.
  * Every other table either boot app looks up is optional: MCFG, SPCR, DBG2, CSRT, BGRT, SRAT, NFIT, PPTT, TPM2, WPBT,
    iBFT, PRMT, OEM0, BBRT, SDEV.
* **How the tables are found.** Through the EFI configuration table: the ACPI 2.0 GUID first, then the 1.0 GUID.
  * The RSDP must have revision ≥ 2 and a valid `XsdtAddress`. RSDT is only used when `Revision == 0`.
  * Neither boot app checks table checksums.
  * Lookup by signature returns the first match (bootmgr `0x101bbdf0`, winload `BlUtlGetAcpiTable 0x1801ad660`).
* **FADT fields read by the boot apps.**
  * The `HypervisorVendorIdentity` at offset 0x10C is read only if `Revision ≥ 6`.
  * If it equals `"MsHyperV"`, bootmgr issues Hyper-V hypercalls (`HVC #1`), and winload and the kernel use them too.
    **It must not be "MsHyperV".** Use `"QCOM"`, which is what the real SC7280 laptop uses. The kernel explicitly treats
    `QCOM` as "no hypervisor": `HviIsAnyHypervisorPresent 0x1405c6ed0`.
  * `ArmBootArch` (offset 129): `PSCI_COMPLIANT` set and `PSCI_USE_HVC` clear enables the boot apps' PSCI SMCs. These
    are PSCI_VERSION and PSCI_FEATURES, plus reset/off and MEM_PROTECT.
  * Preferred_PM_Profile (offset 45, Tablet) and the OEM ID only change menus and log strings.
* **Exception level.** EL1 (under Gunyah) and EL2 (Gunyah removed) are both supported; nothing requires EL2.
  * bootmgr records `StartedInEL2 = (CurrentEL == 2)` at `0x1003a8e4..0x1003a92c`.
  * When started at EL2, both apps drop themselves to EL1 and keep their own EL2 stub, reached with `HVC #1`, for
    firmware calls. Only in that case does winload set up the "microvisor", DRTM and Qualcomm Secure Launch.
* **UEFI services.**
  * Required: BlockIo on the boot medium (bootmgr carries its own FAT, NTFS, UDF and WIM file systems), LoadedImage,
    DevicePath, a console, and GetMemoryMap, ExitBootServices and SetVirtualAddressMap.
  * Optional, absence tolerated: GOP, RNG, TCG2/TCG, EFI_MEMORY_ATTRIBUTE protocol, PCI root bridge, SMBIOS,
    MEMORY_ATTRIBUTES_TABLE and ESRT.
  * GOP must report PixelFormat 1 (BGRX). Format 0 (RGBX) is rejected (`0x1800b5224`). Our `Gop.c:214` already uses
    BGRX.
* **New risks this analysis found.** Neither can be tested without the board.
  1. bootmgr reads `PMCCNTR_EL0` at EL1 directly after the FACP check (`0x101af7c0`). winload writes `PMCR_EL0`,
     `PMCNTENSET_EL0`, `PMCCFILTR_EL0` and `PMUSERENR_EL0` (`0x180047288..0x1800472e0`). If Gunyah traps PMU access,
     the next failure after FACP is an exception.
  2. The kernel needs `CNTFRQ_EL0 != 0` (`0x14049f328`), `PSCI_VERSION` major version 1 (`0x1404a7d8c`), and the GTDT
     **virtual** timer GSIV (offset 64).
* **Minimum set to reach a running installer kernel.**
  * ACPI: RSDP 2.0, XSDT, FADT (rev 6, HW_REDUCED + LOW_POWER_S0, PSCI over SMC, HV id "QCOM"), MADT (8×GICC +
    GICD v3 + GICR), GTDT and a minimal DSDT (8× `ACPI0007`). DBG2 and SPCR (subtype 0x0013 at 0x994000) are strongly
    recommended for debugging.
  * Boot medium: WinPE boots from a RAM disk (`boot.wim`, BCD in §7), so no storage driver is needed to reach Setup.
    Seeing anything needs the GOP framebuffer to survive ExitBootServices, which `MdssDisplayDxe` currently tears
    down.

## 1. Inputs, method, tools

| File (under `workspace/windows-research/bin/`) | Version | sha256 (prefix) |
|---|---|---|
| `efi/boot/bootaa64.efi` (ISO) | 10.0.28000.317 | 3dca6d137e7b61e2 |
| `wim2/2/Windows/System32/winload.efi` (boot.wim index 2; identical to `System32/Boot/winload.efi`, which the BCD loads) | 10.0.26100.8036 | cce44a3beddbfb9a |
| `wim2/2/Windows/System32/ntoskrnl.exe` | 10.0.26100.8036 | 5bb49f8383e38b26 |
| `wim2/2/Windows/System32/kdcom.dll` | 10.0.26100.1882 | edf8fb0a0834fa89 |
| `efi/microsoft/boot/bcd`, `bootmgr.efi`, `sources/boot.wim`, plus `hal.dll`, `hvloader.dll`, `acpi.sys`, `ci.dll` (extracted, not analysed in depth) | | |

Tools, in `tools/`. They run in the `flange-build` image; I used a long-lived container, `winre`.

* `pe_annotate.py <pe> <prefix>` disassembles only code:
  * It takes function ranges from `.pdata`, using both packed and xdata FunctionLength. Leaf functions with no `.pdata`
    are recovered by recursive descent from `bl` targets, exports and the entry point.
  * All other bytes are zeroed, because objdump asserts on data. The result is run through
    `objdump -b binary --adjust-vma`.
  * Annotations:
    * `adrp+add`/`ldr`/`adr` targets, with the ASCII or UTF-16 string or known EFI GUID at the target
    * literal-pool loads, with `'4CC'` ACPI signatures and NTSTATUS values
    * export names, `call <name>`, and the CFG guard pattern (`bl guard; blr x15` → "indirect")
  * Outputs `dis/<name>.asm`, `.xref` (strings/GUIDs/4CCs → sites), `.addrxref`, `.calls` (callee ← callers),
    `.strings` and `.funcs`. Generated for bootmgr, winload, ntos and kdcom.
* `showfn.sh <asm> <addr>` prints one function. `dr.sh <pe> <start> <stop>` gives raw objdump of a range, for jump
  tables.
* `exports.py` dumps the export table (`dis/winload.exports`, `dis/ntos.exports`).
* `guidscan.py` finds raw GUID bytes anywhere in an image, including `.data`, and their code references.
* `bcddump.py` is a minimal regf parser for BCD hives.
* `peutil.py` has small helpers: GUID at an address, jump-table decode.

## 2. How bootmgr and winload find ACPI tables

* **Firmware-table getter.**
  * bootmgr `fn_10053f18(type, &out)` and winload `BlFwGetSystemTable 0x1800494d0` use the same switch on `type`, 1..18.
    The jump table is at `0x100541b0`.

    | type | what is returned |
    |---|