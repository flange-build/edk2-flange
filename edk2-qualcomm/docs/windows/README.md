# Windows 11 on Arm on the RUBIK Pi 3: research

Status, October 2026: **researched, not implemented, and set aside.** The firmware does not boot Windows.
These notes record what we found, so the work can be picked up again. The summary in Chinese is in
[CONCLUSIONS.md](CONCLUSIONS.md).

## Why Windows does not start

The Windows 11 25H2 ARM64 boot manager stops with `BlInitializeLibrary failed 0xc0000225`.
`BlInitializeLibrary` looks up the FADT (`FACP`) through the ACPI RSDP and XSDT, and a missing FADT is fatal.
This firmware hands the OS a device tree and no ACPI tables.

The `ConvertPages: failed to find range 102000 - 102FFF` line logged just before is unrelated. The boot manager
reserves a page at 0x102000 on entry and does not check the result.

## What Windows support would take

- **ACPI tables.** At least RSDP/XSDT, FADT (hardware-reduced, PSCI), MADT (GICv3), GTDT and a DSDT, with a
  setting that chooses between the device tree, ACPI, or both.
- **Display.** HDMI kept scanning out after ExitBootServices, for Windows' basic display driver.
- **Storage and USB.** Windows' own drivers cover UFS (`ACPI\QCOM24A5`), the USB 2.0 port (`PNP0D10`) and,
  once PCIe0 is made ECAM-compatible, the Renesas USB 3.0 controller and its Ethernet.
- **Platform services.** Runtime variable writes, RTC, RNG and Secure Boot.
- **Qualcomm drivers.** GPU, DSPs, power management (PEP) and the 40-pin header need Qualcomm's Windows drivers,
  whose source and licence are open questions.

The Radxa Dragon Q6A, with the same QCS6490, runs Windows 11 this way: Windows' own drivers first, then Radxa's
driver pack.

Hard limits on this board:
- no Windows driver for the on-board Wi-Fi (AP6256 over SDIO);
- only a basic framebuffer on HDMI, which goes through the LT9611 bridge;
- no power management without Qualcomm's PEP driver;
- no TPM;
- non-ECAM PCIe;
- every UFS LUN shows up as a disk, the boot firmware ones included.

## Documents

| File | Contents |
|---|---|
| [PLAN.md](PLAN.md) | Phased plan (phases 0-6), work items, risks and the decisions left to the owner (section 7) |
| [CONCLUSIONS.md](CONCLUSIONS.md) | Summary in Chinese |
| [win-requirements.md](win-requirements.md) | Windows 11 on Arm firmware requirements: ACPI tables, UEFI services, install checks, debugging |
| [bootmgr-winload.md](bootmgr-winload.md) | What the boot manager and the OS loader require (incomplete: its requirement table was not finished) |
| [firmware-gaps.md](firmware-gaps.md) | What this firmware lacks, with designs taken from edk2-rockchip |
| [board-peripherals.md](board-peripherals.md) | RUBIK Pi 3 peripherals and their Windows driver situation |
| [prior-art.md](prior-art.md) | Windows on SC7280/SM7325/QCS6490 elsewhere: Radxa Q6A, Project Silicium, public ACPI dumps, licensing |
| [q6a-firmware.md](q6a-firmware.md) | Radxa Dragon Q6A firmware and the Radxa Q8B Windows driver pack |

The reports were written during the research. They cite local working files (`bin/`, `dis/`, `q6a-fw/`,
`fwgap/`, `prior-art-acpi/`, `win-reqs/`, `tools/`) that hold extracted binaries, disassembly and third-party
ACPI dumps. Those files are not part of the repository and can be regenerated from the Windows ISO and the
Radxa images. Path placeholders such as `<linux-7.0.2>` and `<downloads>` stand for local source trees and
downloads. Statements marked **INFERRED** were not confirmed by a source or on hardware.
