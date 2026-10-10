# EDK2 UEFI firmware for Qualcomm platforms

[简体中文](README.md) | **English**

This directory holds UEFI support for Qualcomm boards. It builds on the
upstream [edk2-platforms](https://github.com/tianocore/edk2-platforms) port
of the Qualcomm Dragonwing RB3 Gen 2 (`Platform/Qualcomm/RB3Gen2`), and
reuses its silicon code (`Silicon/Qualcomm`) in place.

## Supported platforms

| Board | SoC | Config |
|---|---|---|
| Thundercomm RUBIK Pi 3 | QCS6490 | `rubikpi3` |

## Status

The firmware replaces the stock Qualcomm UEFI in the `uefi_a` / `uefi_b`
partitions. XBL, TrustZone, the hypervisor and the rest of the boot chain
come from the board's matching boot firmware package. This ELF is a UEFI
update, not a complete boot firmware or OS image.

For the currently validated official Ubuntu installer configuration, start
with [Ubuntu Desktop installer](#ubuntu-desktop-installer).

Working:

- Serial console on the debug UART (uart5, 115200 8N1), with the setup UI
  (press ESC or F2 during the countdown), in English or Simplified Chinese.
- HDMI (DSI0 -> Lontium LT9611 bridge) at 1920x1080@60 as a GOP: boot logo,
  console and setup UI, with Chinese glyphs. On RubikPi3 the pipeline stays
  active for Linux `simpledrm`, see [Display handoff](#display-handoff).
- UEFI settings (language, boot order, timeout, Hypervisor, DSP Preload) kept on UFS, see
  [Settings](#settings).
- UFS: every LUN gets a boot option; GRUB on the ESP boots Linux.
- The QUP serial engine firmware (`qupfw_a`) is loaded for OS drivers that
  use I2C, SPI and the Bluetooth UART.
- Optional ADSP/CDSP preload for compatible kernels at EL2; see
  [DSPs at EL2](#dsps-at-el2). Keep it **Disabled** for the official Ubuntu
  installer configuration validated below.
- USB host on the three Type-A ports, for keyboards and disks, booting
  from them included: the USB 2.0 port (the SoC's usb_2) and the two USB 3.0
  ports (a Renesas uPD720201 on PCIe0), see [USB](#usb).
- The full DRAM (8 GiB), read from the RAM partition table XBL leaves in
  SMEM, with the firmware carve-outs reserved.
- The mainline RUBIK Pi 3 device tree, handed over to the OS.
- SMBIOS, including the memory records the setup UI shows.

The current board overrides were validated on 2026-10-10 with the
BOOT.MXF.1.0.c1-00430 boot firmware, Hypervisor **EL2**, DSP Preload
**Disabled**, and the official Ubuntu Desktop 26.04.1 ARM64 installer
(kernel `7.0.0-30-generic`). The original GRUB entry reached the desktop
without extra parameters. UFS, the installer USB, GNOME and the installer
processes were checked, and physical HDMI output was confirmed.

Earlier project tests used the flange vendor image (Thundercomm 6.6.90,
EL1) and the flange mainline image (Linux 7.0.2, EL2 with DSP preload).
Those results predate the current installer-specific DT overrides and do
not establish that all peripherals still work with this configuration.

Not supported yet: networking in UEFI (the Ethernet port is an ASIX
AX88179 on the USB 3.0 controller), the USB-C port in UEFI, PCIe1 (the
M.2 slot); display modes other than 1080p60; USB-C DisplayPort; Windows,
which needs ACPI tables (see the research in [docs/windows](docs/windows/)). Variables the OS writes at runtime are not
kept (see [Settings](#settings)).

## Ubuntu Desktop installer

1. Flash the current UEFI image using the [procedure below](#flashing).
2. Enter **Device Manager → Platform Configuration** (设备管理器 → 平台配置).
   Set **Hypervisor = EL2 (KVM)** and **DSP Preload = Disabled**, save with
   F10 and reboot. DSP Auto is not the validated configuration.
3. Select the ARM64 installer USB in Boot Manager, then choose
   **Try or Install Ubuntu** in GRUB without editing its command line.

With the bundled device tree, `clk_ignore_unused`, `pd_ignore_unused`,
`module_blacklist=qcom_ice` and `modprobe.blacklist=qcom_ice` are unnecessary.
The board DT disables ICE and GPI0/GPI1 and removes the UFS dependency on
ICE, avoiding the probe failures seen with the installer kernel while
keeping UFS available.

This verifies the Live desktop and installer startup, not installation to
UFS. The installer has previously damaged the LUN4 boot partition table.
UFS exposes several disks to Linux: identify the OS target by capacity and
partition layout, and keep the boot firmware LUNs out of the installation
plan. Back up the boot partitions and `logfs` before changing disk layouts.
Device names such as `/dev/sda` are not stable identifiers.

If boot fails before any Linux output with DSP Auto, restore **DSP Preload
= Disabled** and reboot. This setting makes UEFI leave Gunyah early and
run at EL2. The DSP preload / delayed Gunyah exit path is still under
investigation for this installer; its precise failure is not yet isolated.

For a diagnostic boot only, serial logging can be added in GRUB with
`console=ttyMSM0,115200n8 earlycon=qcom_geni,0x00994000 loglevel=8`.
These options are not required for normal boot.

## Display handoff

RubikPi3 enables `PcdDisplayHandoff`. `MdssDisplayDxe` retains its fixed
1080p60 DPU/DSI/LT9611 pipeline at ExitBootServices and requests a persistent
MDSS SMMU bypass. The framebuffer memory is reserved. `DspPreloadDxe`
populates the board's `simple-framebuffer` template from GOP before the
OS loader runs, even when DSP preload is disabled; its DT fixup protocol
also handles a loader-provided copy of the template.

The board-specific
[`qcs6490-thundercomm-rubikpi3-mainline.dts`](Platform/Thundercomm/RubikPi3/DeviceTree/qcs6490-thundercomm-rubikpi3-mainline.dts)
includes the base board tree and applies the handoff policy:

- Native MDSS, DISPCC and LT9611 nodes are disabled to preserve the running
  firmware pipeline. The upstream bridge driver does not handle the board's
  single B input configuration used here.
- Display rails stay on independently of framebuffer probe. The framebuffer
  holds GCC display clocks and HF0/HF1 MMU TBU power domains; it can bind
  before Linux disables unused resources instead of waiting for regulator
  modules to load.
- GPIO83 retains `output-high` without `input-disable`, avoiding the pinctrl
  transition that can interrupt the LT9611 power enable.

In the diagnostic run without either ignore-unused parameter, simpledrm
bound at about 18 seconds, ahead of the normal unused clock/domain cleanup
at about 23 seconds. The next boot used the unedited installer entry and
reached the desktop with HDMI output.

This firmware framebuffer path has not been validated for native modesetting,
other resolutions or suspend/resume. A replacement DTB must provide the necessary
handoff resources; the Ubuntu result applies to the bundled board tree.

Ubuntu 26.04.1 installed on UFS with `7.0.0-38-generic` can use the separate
[RubikPi3 MSM DKMS package](linux/rubikpi3-msm-dkms/README.en.md) to fix Adreno 643
rendering at EL2. It uses Ubuntu Mesa Freedreno / Turnip while `simpledrm` keeps
HDMI scanout. See the module guide for scope, installation and rollback.

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
  Gunyah guest, as the stock firmware boots them, so here it is a setting,
  which by default follows `xbl_config` as the stock firmware does (see
  below).
- **SMMU.** Under Gunyah the apps SMMU faults DMA from any stream the guest
  has not set up, and Gunyah takes the system down for a crash dump on the
  first UFS command. Upstream never meets this, since it always removes
  Gunyah. `SmmuDxe` lets the UFS, display and USB streams bypass stage 1
  the way Linux does under the Qualcomm hypervisor, and puts the stream
  entries back at ExitBootServices unless a client requests persistent
  handoff. The framebuffer and preloaded DSPs use that handoff path.
- **SMBIOS** describes the board, the cores with their own frequencies, and
  the memory.
- **Boot menu.** Every device is connected before booting, so the UFS LUNs
  show up in the boot manager, and the countdown is 5 seconds, as on the
  Rockchip platforms.
- **Serial input.** A polling bug made every received character wait
  10 ms, see `edk2-platforms-patches/`.
- **ext4.** Ext4Dxe rounded the number of block groups down, so it could
  not mount a file system smaller than one block group (128 MiB with 4 KiB
  blocks), such as the `usb_fw` partition, see `edk2-platforms-patches/`.
- The device tree includes the mainline sources in
  `devicetree/mainline/upstream`, with board-local boot and display overrides
  in `Platform/Thundercomm/RubikPi3/DeviceTree/`.

## Building

### Prerequisites

Use the shared [build prerequisites](../README.en.md#building). Qualcomm also
needs the Python `cryptography` module for `qtestsign`:

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

The normal build applies patchsets to upstream submodules and may reset
and clean those working trees. Once patchsets are applied, preserve local
submodule work with:

```sh
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE --skip-patchsets
```

Do not use this option to skip the required initial patch application on
a fresh checkout. The repository's GitHub Actions matrix currently builds
Rockchip boards only; RubikPi3 is built and validated separately.

Build options are passed with `--edk2-flags`, for instance:

```
./edk2-qualcomm/build.sh -d rubikpi3 --edk2-flags "-D EXIT_GUNYAH=TRUE"
```

| Option | Default | Meaning |
|---|---|---|
| `EXIT_GUNYAH` | `FALSE` | Exception level when neither the Hypervisor setting nor `xbl_config` says which: `TRUE` tears Gunyah down and runs UEFI and the OS at EL2 (the upstream RB3 Gen 2 behaviour). |
| `DEFAULT_LANG` | `en-US` | Language of the menus until one is picked under Select Language: `en-US` or `zh-Hans`. |

## Exception level

XBL always starts UEFI at EL1 under the Gunyah hypervisor. First thing,
before there is a console, UEFI decides whether to keep Gunyah or to have
it torn down and continue at EL2, with the same SMC the stock UEFI uses. It
decides from, in this order:

1. The **Hypervisor** setting in Device Manager > Platform Configuration
   (平台配置 > 虚拟机监控程序): EL1 (Gunyah) or EL2 (KVM). It takes effect
   at the next reset.
2. When it is Auto (the default): the `OsConfigTableSelection` property of
   `xbl_config`, as the stock UEFI does. `xbl_config.elf` says 1 (Gunyah,
   EL1) and `xbl_config_kvm.elf`, which flange's mainline product flashes,
   says 2 (KVM, EL2). XBL leaves the device tree it comes from in memory and
   its address in the shared IMEM cookie at 0x146AA000 (+0x58, size at +0x60).
3. When that cannot be read either: the `EXIT_GUNYAH` build option.

For EL2, Gunyah leaves either right away, or, when the DSPs are to be
preloaded, only once the OS loader's ExitBootServices has succeeded, which
is also when the stock UEFI makes the call (see [DSPs at EL2](#dsps-at-el2)).
Gunyah takes one such call per boot, so a boot that keeps Gunyah until then
does not confirm it at the start the way an EL1 boot does.

The console says what was decided and why, in `RELEASE` builds too, for
instance:

```
QCS6490: hypervisor: setting Auto, xbl_config KVM -> EL2 (KVM)
QCS6490: hypervisor: Gunyah stays until ExitBootServices (DSP preload)
```

The Platform Configuration page shows the same. The stock UEFI stops when
TrustZone rejects the call; this firmware warns and carries on at EL1.

The vendor kernel needs EL1 for its remote processors (ADSP, CDSP, video).
The tested mainline kernels use EL2 for KVM. Whether a kernel can adopt
preloaded DSPs is configuration-dependent; use DSP Disabled for the
official Ubuntu installer described above.

## DSPs at EL2

Without Gunyah, Linux cannot start the ADSP and CDSP on this board: it asks
TrustZone for a resource table this TrustZone does not provide. Linux 7.0
does attach to DSPs the boot firmware started (it checks ready and handover
in their SMP2P entries when it probes), as on the Radxa Dragon Q6A. When enabled,
`DspPreloadDxe` starts them at ReadyToBoot, the way Linux's `qcom_q6v5_pas`
would: firmware from the OS's root file system (`/usr/lib/firmware`,
partition `PcdDspFirmwarePartition`, ext4 through Ext4Dxe), the AOP
`load_state` message, proxy votes for their power rails and the CDSP's path
to memory, the SMP2P entries Linux's SMP2P driver would create, and the
TrustZone PAS calls. It waits for ready and handover.

TrustZone only runs DSPs it starts for a Gunyah guest: at EL2 it accepts
every PAS call, but the DSPs never run. A boot that runs the OS at EL2 with
the DSPs preloaded therefore keeps Gunyah until ExitBootServices, as the
stock UEFI does anyway: `GunyahExitDxe` wraps ExitBootServices and, once it
has succeeded, has Gunyah leave and carries on at EL2 with UEFI's
translation tables, before returning to the OS loader. Gunyah wipes the SMMU
stream entries it does not leave in bypass, so `SmmuDxe` then sets up those
of the DSPs, which Linux adopts in bypass; and the `iommus` of their
remoteproc nodes are removed from the device tree GRUB loads (through
`EFI_DT_FIXUP_PROTOCOL`), as Linux would put the running DSPs in an empty
SMMU domain otherwise.

The **DSP Preload** setting (预加载 DSP) picks Auto (when the OS runs at
EL2), Disabled or Always (at EL1 too), and each DSP; SEC reads it too, so it
takes effect at the next reset. Disabled brings back a boot that leaves
Gunyah first thing. `PcdGunyahLateExit` changes when Gunyah leaves: 0 always
first thing, 1 (the default) at ExitBootServices when the DSPs are
preloaded, 2 always at ExitBootServices.

The console shows each step, for instance:

```
DspPreload: adsp running after 146 ms
GunyahExit: leaving Gunyah: srtm
HandOverNow: adsp stream 0x1800 mask 0x0: entry 0, SMR 0x80001800, S2CR 0x100FF, kept for the OS
DspPreload: after the Gunyah exit: adsp SMP2P 0x6 (running)
```

`srtm` are the steps of the switch (call made, back at EL2, translation
tables in place, MMU on); a boot that stops after one of them tells where.

## USB

| Port | Controller | Driver |
|---|---|---|
| USB 2.0 Type-A | usb_2, the SoC's secondary DWC3 (USB 2.0 only) | `Dwc3HostDxe` |
| 2x USB 3.0 Type-A, Ethernet | Renesas uPD720201 xHCI on PCIe0 | `Qcs6490PciHostBridgeLib`, `RenesasXhciFwDxe` |
| USB-C | usb_1, the primary DWC3 | none: device mode for EDL and adb |

Nothing before UEFI brings either controller up, so both are set up the
way Linux 7.0 does it: `Dwc3HostDxe` powers usb_2 and its HS PHY and puts
the core in host mode, and `Qcs6490PciHostBridgeLib` powers the supplies
behind PCIe0, the QMP PHY and the root complex, and trains the link
(Gen2 x1) before PciBusDxe enumerates it. XhciDxe then drives both.

The uPD720201 has no EEPROM: Linux downloads its firmware from
`renesas_usb_fw.mem` on the `usb_fw` partition (UFS LUN 3, ext4) at every
boot. `RenesasXhciFwDxe` does the same before XhciDxe starts on it, reading
the file through Ext4Dxe; the firmware is not part of this repository. The
chip keeps it while it has power, so Linux finds it running and skips its
download and the five-second wait before it.

At ExitBootServices PCIe0 is quiesced: link training off and PERST#
asserted, the supplies left on. Linux turns the clocks it does not use yet
off seconds before it probes PCIe0, the link's reference clock among them;
a uPD720201 left running on a live link through that came up at 2.5 GT/s
only. Held in reset, it trains at 5 GT/s under Linux too.

All USB DMA stays below 4 GiB, as the UFS's does: under Gunyah, usb_2 DMA
to a buffer at the top of DRAM timed out. The SMMU lets both controllers'
streams through while UEFI runs (`SmmuDxe`); XhciDxe stops them at
ExitBootServices.

## Settings

UEFI variables, and with them every setting, are kept in the `logfs`
partition of UFS LUN 4, repurposed from the stock UEFI log partition.
Updating only `uefi_a` / `uefi_b` preserves it. A full disk restore,
partition-table rewrite or installer operation can erase it and reset the
settings; back it up before those operations:

- SEC, before deciding the exception level, reads the store into memory at
  0xA0000000 through the UFS controller as XBL left it (it only borrows the
  controller; DXE initializes it again), after letting UFS DMA through the
  SMMU for the moment.
- `NvStoreFvbDxe` gives the standard variable driver that memory, and writes
  every change back to the partition as soon as UFS is up.
- The first boot formats the store. If SEC cannot read the partition, that
  boot keeps its variables in memory only and leaves the partition alone;
  the console and the Platform Configuration page say why.
- Variables the OS writes at runtime are not written back. The firmware says
  so in the `EFI_RT_PROPERTIES_TABLE`, so Linux keeps `efivarfs` read-only.

The partition is set per board, with `PcdNvStoreUfsLun` and
`PcdNvStorePartitionName`.

## Flashing

Use a matching `prog_firehose_ddr.elf` from the board's boot firmware
package and an EDL-capable USB connection. The board must accept the
qtestsign test-signed ELF; this build does not provide production signing
keys. The programmer and other boot firmware are not bundled in this image.

Enter EDL and confirm the host sees Qualcomm **05c6:9008**. A **900e**
RamDump device is a different mode. Read the actual GPT before writing:

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS printgpt --lun 4
```

On the tested layout, `uefi_a` and `uefi_b` are each 5 MiB on LUN4.
Use their partition names rather than hard-coded sector offsets. Back up
both slots and the settings store to new files before updating:

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_a uefi_a-before.bin --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_b uefi_b-before.bin --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part logfs logfs-before.bin --lun 4
```

For the A/B layout above, write and read back each slot:

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS write-part uefi_a RUBIKPI3_UEFI.elf --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_a uefi_a-after.bin --lun 4
cmp -n "$(stat -c%s RUBIKPI3_UEFI.elf)" RUBIKPI3_UEFI.elf uefi_a-after.bin

edl-ng --loader prog_firehose_ddr.elf --memory UFS write-part uefi_b RUBIKPI3_UEFI.elf --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_b uefi_b-after.bin --lun 4
cmp -n "$(stat -c%s RUBIKPI3_UEFI.elf)" RUBIKPI3_UEFI.elf uefi_b-after.bin
```

Proceed only when the commands and comparisons succeed. Then reboot:

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS reset --delay 1
```

To roll back, write the corresponding saved partition image to each slot.
Keep `logfs` unless a settings reset is intended. If LUN4's GPT or the other
boot firmware partitions are missing, an EFI-only update cannot repair the
boot chain: restore the matching board firmware layout first, then replace
UEFI. Full firmware recovery is separate from an ordinary UEFI update.

## Memory map

XBL only loads the UEFI image into 0x9FB00000-0xA0A00000; the stock UEFI
uses 0x9FB00000-0xA0000000. This image loads at 0x9FC00000: the FD takes
0x9FC00000-0x9FF00000, the SEC stack starts at 0x9FF00000, the variable
store (and a status page SEC fills in) takes 0xA0000000-0xA0091000, and PEI
and early DXE run from 0xDC000000-0xE0000000.

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
├── docs/windows/                Research notes on Windows 11 on Arm (not implemented)
├── edk2-platforms-patches/      Fixes to upstream edk2-platforms code
├── misc/qtestsign/              ELF hash segment and test signature (submodule)
├── Platform/Thundercomm/RubikPi3/
│   ├── RubikPi3.dsc             Board PCDs and device tree
│   ├── RubikPi3.Modules.fdf.inc
│   ├── Drivers/BoardDxe/        LT9611 control lines
│   └── DeviceTree/
│       ├── Mainline.inf
│       └── qcs6490-thundercomm-rubikpi3-mainline.dts
└── Silicon/Qualcomm/QCS6490/    Shared by QCS6490 boards
    ├── QCS6490.dec
    ├── QCS6490.dsc.inc
    ├── QCS6490.fdf
    ├── Library/
    │   ├── Qcs6490Lib/          ArmPlatformLib: EL decision and switch, early
    │   │                        UFS read of the variable store, memory map
    │   ├── Qcs6490NvStatusLib/  What SEC decided, for DXE drivers
    │   ├── Qcs6490RpmhLib/      RPMh votes through the apps RSC
    │   ├── Qcs6490GccLib/       GCC power domains, clocks and resets
    │   ├── Qcs6490TlmmLib/      TLMM pins
    │   ├── Qcs6490PciHostBridgeLib/  PCIe0 bring-up and root bridge
    │   ├── Qcs6490PciSegmentLib/     PCIe0 configuration space
    │   ├── MemoryInitPeiLib/    MMU setup
    │   └── OemMiscLib/          SMBIOS
    └── Drivers/
        ├── SmmuDxe/             SMMU set up for UFS, display and USB DMA under Gunyah,
        │                        display and DSP streams handed over to the OS
        ├── NvStoreFvbDxe/       Variable store FVB, written back to UFS
        ├── PlatformConfigDxe/   Platform Configuration page (Hypervisor, DSP preload)
        ├── MdssDisplayDxe/      HDMI: DPU, DSI, LT9611, GOP
        ├── QupFwDxe/            QUP serial engine firmware for the OS
        ├── DspPreloadDxe/       Optional DSP preload and GOP device-tree fixups
        ├── GunyahExitDxe/       Gunyah leaves at ExitBootServices
        ├── Dwc3HostDxe/         USB 2.0 port (usb_2) in host mode
        ├── RenesasXhciFwDxe/    uPD720201 firmware from the usb_fw partition
        └── SmbiosMemoryDxe/     SMBIOS memory records
```

The boot logo (`edk2-common/Drivers/LogoDxe`) and the Chinese font
(`edk2-rockchip/Silicon/Rockchip/Drivers/CjkFontDxe`) are shared with the
Rockchip platforms.
