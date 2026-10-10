# RubikPi3 MSM DKMS

[简体中文](README.md) | **English**

`rubikpi3-msm-dkms` packages the Adreno 643 EL2 workaround for RubikPi3.
It includes MSM sources from Ubuntu `7.0.0-38.38` and builds a replacement
`msm.ko` on the device. Keep Ubuntu's Mesa Freedreno / Turnip userspace drivers.

## Scope

- Thundercomm RubikPi3, Ubuntu ARM64, UEFI **EL2 (KVM)**, Linux VHE host.
- UEFI `simpledrm` provides HDMI scanout; MSM exposes a separate GPU render node.
- Automatic builds are limited to `7.0.0-*-generic` on `aarch64`. Other kernel
  series are skipped. New ABIs within this series may still need source changes.
- This package conflicts with `kgsl-dkms`; proprietary qcom-adreno / KGSL is not
  part of this setup.

The workaround uses the existing direct SECVID switch only when the device-tree
compatible is `thundercomm,rubikpi3` and the kernel currently executes at EL2.
EL1, nVHE and other boards retain the original path. No GRUB arguments are needed;
`zap_direct_el2=0` disables the workaround for diagnosis. The packaged modprobe
configuration uses only the stock-compatible `separate_gpu_kms=1` option, so
uninstallation cannot leave an unknown custom parameter behind.

## Installation

```sh
sudo apt install dkms build-essential python3 linux-headers-$(uname -r)
sudo apt install ./rubikpi3-msm-dkms_0.1.0_arm64.deb
dkms status
modinfo -n msm
sudo reboot
```

The installed module normally resides at
`/lib/modules/$(uname -r)/updates/dkms/msm.ko.zst`. Installation and removal refresh
existing initramfs images. Ubuntu's DKMS and initramfs hooks handle new kernels;
matching headers are required. Keep a working kernel available if a build fails.
Secure Boot enforcement requires enrollment of the local DKMS signing key and
has not been validated in this setup.

For migration from a manual installation, first back up
`updates/rubikpi3/msm.ko` outside `/lib/modules`, and remove the old
`zap_direct_el2=1` option from `/etc/modprobe.d/rubikpi3-gpu.conf`.
Keeping `separate_gpu_kms=1` is fine. These changes affect files on disk; reboot
only after successful installation and initramfs generation.

## Verification

```sh
dkms status
modinfo -n msm
sudo cat /sys/module/msm/parameters/zap_direct_el2
eglinfo -B -p surfaceless
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json vulkaninfo --summary
sudo journalctl -k -b | grep -E 'SECVID|adreno|msm|fault|hangcheck'
```

Expected renderers: `FD643` for OpenGL and `Turnip Adreno (TM) 643` for Vulkan.
Also test actual rendering: successful device enumeration alone is insufficient.
HDMI scanout remains on `simpledrm`; this package does not fix native DPU mode
setting or suspend/resume.

## Removal

```sh
sudo apt remove rubikpi3-msm-dkms
sudo reboot
```

DKMS restores its backup of the stock MSM module, and the package manager removes
the packaged modprobe configuration and refreshes initramfs. The stock driver
may fall back to software rendering; removal does not fix its GPU problem.
Do not unload MSM live: `rmmod msm` previously caused a kernel crash on this
device. Reboot to switch drivers. Manual backups are not automatically restored.

## Building the package

On Linux with Python 3.11+, `dpkg-deb` and `patch`:

```sh
apt download linux-source-7.0.0=7.0.0-38.38
python3 build-package.py linux-source-7.0.0_7.0.0-38.38_all.deb --output dist
```

The script verifies a pinned SHA256, extracts MSM, required internal DRM headers
and license texts, applies `patches/`, and creates an ARM64 source package. No
cross compiler is needed on the packaging host. The firmware repository does
not vendor the complete kernel source; the resulting deb contains the sources
needed for DKMS. Patches and packaging scripts use GPL-2.0-only. Upstream files
retain their SPDX licenses, with texts in the packaged
`/usr/src/rubikpi3-msm-0.1.0/LICENSES/` directory.

See the [DKMS 3.2.2 documentation](https://github.com/dkms-project/dkms/blob/v3.2.2/dkms.8.in)
for configuration and automatic build semantics.

## Device validation

Before packaging, the equivalent patch passed FD643 EGL rendering/readback,
Turnip enumeration and GNOME Shell GPU-use checks on Ubuntu 26.04.1 /
`7.0.0-38-generic`. See [validation notes](VALIDATION.md) for DKMS package test results.
