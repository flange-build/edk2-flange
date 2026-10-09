# edk2-flange

[简体中文](README.md) | **English**

UEFI firmware for Rockchip RK3588 and Qualcomm QCS6490 boards, built on
[TianoCore EDK II](https://github.com/tianocore/edk2). The project brings
UEFI setup, boot management and board-specific hardware initialization to
these ARM64 platforms, with shared boot branding and English / Simplified
Chinese menus.

Rockchip support builds on
[edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588). Qualcomm support
builds on the RB3 Gen 2 port in
[edk2-platforms](https://github.com/tianocore/edk2-platforms), with local
QCS6490 drivers and a Thundercomm RUBIK Pi 3 platform.

## Platforms and guides

| Platform | Build entry point | Output | Guide |
| --- | --- | --- | --- |
| Rockchip RK3588 / RK3588S boards | `./build.sh -d <device>` | `RK3588_NOR_FLASH.img` | [Rockchip setup, board list and OS compatibility](edk2-rockchip/README.en.md) |
| Thundercomm RUBIK Pi 3 (QCS6490) | `./edk2-qualcomm/build.sh -d rubikpi3` | `RUBIKPI3_UEFI.elf` | [Qualcomm setup, flashing and hardware status](edk2-qualcomm/README.en.md) |

Rockchip device names are defined in [configs](configs/); Qualcomm device
names are defined in [edk2-qualcomm/configs](edk2-qualcomm/configs/).
Firmware images and flashing procedures are specific to each platform.

## Current RubikPi3 validation

As of 2026-10-10, the official **Ubuntu Desktop 26.04.1 ARM64** installer
(kernel `7.0.0-30-generic`) boots from USB to the Live desktop and installer
using the **original, unedited GRUB entry**. UFS storage is also detected.
No `clk_ignore_unused`, `pd_ignore_unused` or module blacklist is needed
with the bundled board device tree.

Use these settings under **Device Manager → Platform Configuration**, save
and reboot:

| Setting | Validated value |
| --- | --- |
| Hypervisor | **EL2 (KVM)** |
| DSP Preload | **Disabled** |

The HDMI path preserves UEFI's 1920×1080@60 DPU/DSI/LT9611 configuration
and hands its framebuffer to Linux `simpledrm`. The device tree keeps its
supplies on and lets Linux claim display clocks and power domains before
unused-resource cleanup. This validates firmware framebuffer output;
native display modesetting, GPU acceleration and suspend/resume are not
established by this test.

The board overrides disable ICE and GPI nodes that caused failures with
the installer kernel, while retaining UFS access. DSP preload remains
available for other configurations, but the Ubuntu installer boot with
DSP Auto did not reach Linux in our recent tests. Keep it disabled for
the validated setup; the internal failure is still under investigation.

This result covers **booting the Live environment**, not a complete OS
installation. The installer previously damaged the board's LUN4 boot
partitions. Review target disks and partition changes before installing;
do not erase firmware LUNs or treat all exposed UFS disks as OS storage.
See the [Qualcomm guide](edk2-qualcomm/README.en.md#ubuntu-desktop-installer)
for the current procedure and limitations.

## Building

Build on Linux, or use a Linux environment such as WSL. Both build scripts
use the repository's pinned submodules and local patchsets.

### Get the sources

```sh
git clone --recursive https://github.com/flange-build/edk2-flange.git
cd edk2-flange
```

For an existing checkout, initialize missing submodules with
`git submodule update --init --recursive` after saving any local submodule
work.

### Dependencies

On Debian / Ubuntu, the following covers the tools used by both platform
builds, including Qualcomm ELF packaging:

```sh
sudo apt install acpica-tools binutils-aarch64-linux-gnu build-essential \
  device-tree-compiler gcc-aarch64-linux-gnu gettext git \
  libc6-dev-arm64-cross python3 python3-cryptography python3-pyelftools uuid-dev
```

### Build a board

Run one of these from the repository root:

```sh
# Qualcomm: Thundercomm RUBIK Pi 3
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE

# Rockchip: Radxa ROCK 5B
./build.sh -d rock-5b -r RELEASE
```

Both default to `DEBUG` when `-r` is omitted. Outputs are copied to the
repository root; intermediate files live under `workspace/`. Rockchip
builds use the open-source TF-A submodule by default. Run either script
with `--help` for its platform-specific options.

**When working on submodules:** the normal patchset step can run
`git reset --hard` and `git clean -xfd` in its target submodules. Once the
required patchsets have been applied, use `--skip-patchsets` for development
builds to preserve local work:

```sh
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE --skip-patchsets
./build.sh -d rock-5b -r RELEASE --skip-patchsets
```

Do not skip patchsets on a fresh checkout that has not had them applied.

The checked-in [GitHub Actions build matrix](.github/workflows/build.yml)
currently covers Rockchip boards. Qualcomm build and hardware validation
are performed separately; the release workflow does not currently produce
RubikPi3 artifacts.

## Flashing and first boot

Follow the guide for the image you built:

- **Rockchip:** flash the board-specific image to SPI NOR, SD or eMMC;
  see [installation and updates](edk2-rockchip/README.en.md#getting-started).
- **RubikPi3:** keep the matching Qualcomm boot firmware and flash the UEFI
  ELF to the identified `uefi_a` / `uefi_b` partitions through EDL. Back up
  the partitions and read back the written image. See
  [Qualcomm flashing](edk2-qualcomm/README.en.md#flashing).

Serial console settings also differ: **115200 8N1** on RubikPi3's debug
UART, **1500000 8N1** on RK3588 UART2. Use a UTF-8 terminal with CJK fonts
for the Simplified Chinese menus.

## Repository layout

| Path | Purpose |
| --- | --- |
| `edk2/`, `edk2-platforms/`, `edk2-non-osi/` | Upstream EDK II submodules |
| `edk2-common/` | Shared boot logo |
| `edk2-rockchip/`, `configs/`, `build.sh` | Rockchip platforms, configuration and build |
| `edk2-qualcomm/` | Qualcomm platforms, silicon drivers, build script and documentation |
| `edk2-patches/`, `edk2-qualcomm/edk2-platforms-patches/` | Local patches applied to upstream EDK II sources |
| `devicetree/` | Mainline and vendor device trees and mainline patchsets |
| `arm-trusted-firmware/`, `arm-trusted-firmware-patches/` | TF-A source and patches for Rockchip |
| `misc/`, `edk2-rockchip-non-osi/` | Rockchip packaging tools and binary components |
| `workspace/` | Generated build files; ignored by Git |

The RubikPi3 board device tree is
[`qcs6490-thundercomm-rubikpi3-mainline.dts`](edk2-qualcomm/Platform/Thundercomm/RubikPi3/DeviceTree/qcs6490-thundercomm-rubikpi3-mainline.dts).
It includes the base board tree and carries the firmware handoff and
installer compatibility overrides. Runtime framebuffer address and mode
are populated from GOP by the firmware.

## Reporting issues

Open an issue in
[flange-build/edk2-flange](https://github.com/flange-build/edk2-flange/issues)
with the board, commit, build mode, boot firmware package, OS/kernel,
UEFI settings, reproduction steps and serial log. Distinguish failures in
UEFI, the OS loader, the kernel and the desktop when possible.

Qualcomm Windows / ACPI work is documented under
[Windows research](edk2-qualcomm/docs/windows/README.md); it is not currently
a supported RubikPi3 boot path. Rockchip OS support is documented in its
own [platform guide](edk2-rockchip/README.en.md#supported-oses).

## Licenses and credits

Most firmware code uses **BSD-2-Clause-Patent**, with other licenses on
individual components. Check each file's SPDX identifier and the license
files in the corresponding submodules. Some imported code is **GPL-2.0**;
firmware blobs have their own redistribution terms.

Thanks to TianoCore, the edk2-rk3588 contributors, the upstream Qualcomm
RB3 Gen 2 port, Linux device-tree contributors, qtestsign and the board
communities whose work this project builds on. Rockchip-specific history
and community links remain in the [Rockchip guide](edk2-rockchip/README.en.md#credits--alternatives).
