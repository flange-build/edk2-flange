# EDK2 UEFI firmware for Qualcomm platforms

This directory holds UEFI support for Qualcomm boards. It builds on the
upstream [edk2-platforms](https://github.com/tianocore/edk2-platforms) port
of the Qualcomm Dragonwing RB3 Gen 2 (`Platform/Qualcomm/RB3Gen2`), and
reuses its silicon code (`Silicon/Qualcomm`) in place.

## Supported platforms

| Board | SoC | Config |
|---|---|---|
| Thundercomm RUBIK Pi 3 | QCS6490 | `rubikpi3` |

## Status

The firmware replaces the stock Qualcomm UEFI (`uefi.elf` in the `uefi_a`
partition). XBL, TrustZone, the hypervisor and the rest of the boot chain
stay the stock ones.

Working:

- Serial console on the debug UART (uart5, 115200 8N1), with the setup UI
  (press ESC or F2 during the countdown), in English or Simplified Chinese.
- HDMI (DSI0 -> Lontium LT9611 bridge) at 1920x1080@60 as a GOP: boot logo,
  console and setup UI, with Chinese glyphs. The display is switched off
  again before the OS starts, which drives it itself.
- UFS: every LUN gets a boot option; GRUB on the ESP boots Linux.
- The QUP serial engine firmware (`qupfw_a`) is loaded for the OS, which
  needs it for I2C, SPI and the Bluetooth UART, and so for HDMI.
- The full DRAM (8 GiB), read from the RAM partition table XBL leaves in
  SMEM, with the firmware carve-outs reserved.
- The mainline RUBIK Pi 3 device tree, handed over to the OS.
- SMBIOS, including the memory records the setup UI shows.

Tested on a RUBIK Pi 3 with the BOOT.MXF.1.0.c1-00430 boot firmware and the
flange rubikpi3 image (Thundercomm 6.6.90 kernel): the kernel starts at EL1
under Gunyah, and ADSP, CDSP, video, Wi-Fi and the USB 3 Ethernet come up as
with the stock firmware.

Not supported yet: USB, networking and PCIe in UEFI; display modes other
than 1080p60; USB-C DisplayPort. UEFI variables are kept in memory and do
not survive a reboot.

## What differs from the upstream RB3 Gen 2 port

- **Memory map.** Upstream describes 0x80000000-0xE0000000 as one range of
  plain RAM. The OS then sees 1.5 GiB, and the hypervisor, TrustZone, SMEM
  and remote processor regions inside that range are free for the taking;
  Linux booted through the EFI stub builds its memory map from UEFI alone.
  Here DRAM comes from SMEM and the carve-outs are reserved (see below).
- **The firmware volume is reserved** while UEFI runs from it. Upstream
  leaves `PcdFdBaseAddress` unset, so nothing reserves it.
- **Exception level.** Upstream always asks TrustZone to remove Gunyah and
  continues at EL2. The vendor kernels for these boards expect to run as a
  Gunyah guest, as the stock firmware boots them, so the switch is a build
  option here and off by default (see below).
- **SMMU.** Under Gunyah the apps SMMU faults DMA from any stream the guest
  has not set up, and Gunyah takes the system down for a crash dump on the
  first UFS command. Upstream never meets this, since it always removes
  Gunyah. `SmmuDxe` lets the UFS and display streams bypass stage 1 the way
  Linux does under the Qualcomm hypervisor, and puts the stream entries back
  at ExitBootServices.
- **SMBIOS** describes the board, the cores with their own frequencies, and
  the memory.
- **Boot menu.** Every device is connected before booting, so the UFS LUNs
  show up in the boot manager, and the countdown is 5 seconds, as on the
  Rockchip platforms.
- **Serial input.** A polling bug made every received character wait
  10 ms, see `edk2-platforms-patches/`.
- The device tree is built from the mainline sources in
  `devicetree/mainline/upstream`, instead of a dummy or user-provided DTB.

## Building

### Prerequisites

The same as for the Rockchip platforms (see the top-level README), plus the
Python `cryptography` module for `qtestsign`:

```
sudo apt install acpica-tools binutils-aarch64-linux-gnu build-essential \
  device-tree-compiler gcc-aarch64-linux-gnu git python3 \
  python3-cryptography uuid-dev
```

Check out the submodules, which include
[qtestsign](https://github.com/msm8916-mainline/qtestsign):

```
git submodule update --init --recursive
```

### Build

```
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE
```

This produces `RUBIKPI3_UEFI.elf` in the repository root. `DEBUG` builds
(the default) also log the memory map and every driver to the serial
console.

Build options are passed with `--edk2-flags`, for instance:

```
./edk2-qualcomm/build.sh -d rubikpi3 --edk2-flags "-D EXIT_GUNYAH=TRUE"
```

| Option | Default | Meaning |
|---|---|---|
| `EXIT_GUNYAH` | `FALSE` | Ask TrustZone to tear down Gunyah as soon as UEFI starts, so that UEFI and the OS run at EL2 (the upstream RB3 Gen 2 behaviour). |
| `DEFAULT_LANG` | `en-US` | Language of the menus: `en-US` or `zh-Hans`. Select Language changes it until the next reset only. |

## Exception level

XBL always starts UEFI at EL1 under the Gunyah hypervisor. UEFI then tells
TrustZone whether to keep Gunyah or to tear it down and continue at EL2,
with the same SMC the stock UEFI uses:

| `EXIT_GUNYAH` | UEFI and OS |
|---|---|
| `FALSE` (default) | EL1, Gunyah guest, as the stock firmware boots with the default `xbl_config.elf` |
| `TRUE` | EL2, as the stock firmware boots with `xbl_config_kvm.elf` |

Removing Gunyah has to happen first thing, before there is a console.
Keeping it is confirmed right after the firmware banner. Either way the
console then says how it went, in `RELEASE` builds too, for instance:

```
QCS6490: Keeping Gunyah, running at EL1: confirmed by TrustZone
```

The stock UEFI stops when TrustZone rejects the call; this firmware warns
and carries on at EL1.

The stock UEFI takes this choice from the `OsConfigTableSelection`
property of `xbl_config`, which is what sets `xbl_config.elf` and
`xbl_config_kvm.elf` apart. This firmware does not read it, so which of the
two is flashed makes no difference here.

The vendor kernel needs EL1 for its remote processors (ADSP, CDSP, video).
A mainline kernel at EL2 gets KVM, but see the flange notes on the DSPs.

## Flashing

The image goes into `uefi_a` on UFS LUN 4 (1280 sectors of 4 KiB). Put the
board in EDL mode and, from a directory holding the firehose programmer
(`prog_firehose_ddr.elf` from the boot firmware package) and the image
renamed to `uefi.elf`, write a `rawprogram_uefi.xml`:

```xml
<?xml version="1.0" ?>
<data>
  <program SECTOR_SIZE_IN_BYTES="4096" file_sector_offset="0" filename="uefi.elf"
           label="uefi_a" num_partition_sectors="1280" partofsingleimage="false"
           physical_partition_number="4" readbackverify="false" size_in_KB="5120.0"
           sparse="false" start_byte_hex="0x42c6000" start_sector="17094"/>
</data>
```

and flash it with `edl-ng`:

```
edl-ng --loader prog_firehose_ddr.elf --memory UFS rawprogram rawprogram_uefi.xml
```

To go back, flash the stock `uefi.elf` the same way. The values above are
those of the RUBIK Pi 3 boot firmware package (`rawprogram4.xml`); check
them against the package you use.

## Memory map

XBL only loads the UEFI image into 0x9FB00000-0xA0A00000; the stock UEFI
uses 0x9FB00000-0xA0000000. This image loads at 0x9FC00000: the FD takes
0x9FC00000-0x9FF00000, the SEC stack starts at 0x9FF00000, and PEI and
early DXE run from 0xDC000000-0xE0000000.

DRAM is read from the RAM partition table in SMEM (item 402). If it cannot
be read, the firmware assumes 2 GiB at 0x80000000 and says so on the
console. These carve-outs are reserved, and not mapped in UEFI:

| Range | Owner |
|---|---|
| 0x80000000-0x83600000 | Hypervisor, XBL, AOP, command DB, SMEM, CPUCP, WLAN firmware, CDSP secure heap |
| 0x84300000-0x9AE00000 | Remote processors: camera, WPSS, ADSP, CDSP, SPSS, video, CVP, IPA, GPU, modem |
| 0x9CB80000-0x9D380000 | ADSP RPC remote heap |
| 0xC0000000-0xC3400000 | TrustZone: statistics, tags, QTEE, trusted applications |
| 0xD0600000-0xD0700000 | Debug VM |
| 0xE0000000-0xE0F00000 | DBI dump |

The table is the union of the memory map of the stock Qualcomm UEFI and the
reserved memory of the QCS6490 device trees, mainline and vendor, minus the
splash screen of the stock UEFI. It lives in
`Silicon/Qualcomm/QCS6490/Library/Qcs6490Lib/Qcs6490Mem.c`.

## Layout

```
edk2-qualcomm/
├── build.sh                     Build entry point
├── configs/                     One file per board
├── edk2-platforms-patches/      Fixes to upstream Qualcomm code
├── misc/qtestsign/              ELF hash segment and test signature (submodule)
├── Platform/Thundercomm/RubikPi3/
│   ├── RubikPi3.dsc             Board PCDs and device tree
│   ├── RubikPi3.Modules.fdf.inc
│   ├── Drivers/BoardDxe/        LT9611 control lines
│   └── DeviceTree/Mainline.inf
└── Silicon/Qualcomm/QCS6490/    Shared by QCS6490 boards
    ├── QCS6490.dec
    ├── QCS6490.dsc.inc
    ├── QCS6490.fdf
    ├── Library/
    │   ├── Qcs6490Lib/          ArmPlatformLib: EL switch, memory map
    │   ├── MemoryInitPeiLib/    MMU setup
    │   └── OemMiscLib/          SMBIOS
    └── Drivers/
        ├── SmmuDxe/             SMMU set up for UFS and display DMA under Gunyah
        ├── MdssDisplayDxe/      HDMI: DPU, DSI, LT9611, GOP
        ├── QupFwDxe/            QUP serial engine firmware for the OS
        └── SmbiosMemoryDxe/     SMBIOS memory records
```

The boot logo (`edk2-common/Drivers/LogoDxe`) and the Chinese font
(`edk2-rockchip/Silicon/Rockchip/Drivers/CjkFontDxe`) are shared with the
Rockchip platforms.
