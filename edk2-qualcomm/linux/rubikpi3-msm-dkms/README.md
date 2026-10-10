# RubikPi3 MSM DKMS

**简体中文** | [English](README.en.md)

将 RubikPi3 的 Adreno 643 / EL2 修复打包为 `rubikpi3-msm-dkms`。
包含 Ubuntu `7.0.0-38.38` 的 MSM 源码，在设备上编译为替代原版的 `msm.ko`；
Mesa Freedreno / Turnip 用户态驱动继续使用 Ubuntu 软件包。

## 适用范围

- Thundercomm RubikPi3，Ubuntu ARM64，UEFI **EL2 (KVM)**，Linux VHE 主机。
- 当前设备树使用 `simpledrm` 输出 HDMI；MSM 单独提供 GPU 渲染节点。
- DKMS 自动构建范围为 `7.0.0-*-generic`、`aarch64`，实际验证内核见下方。
  其他内核系列自动跳过；即使同系列，新增 ABI 也可能需要适配源码。
- `qcom-adreno` / KGSL 闭源栈不在此方案内；本包与 `kgsl-dkms` 冲突。

驱动仅在设备树 compatible 为 `thundercomm,rubikpi3`，且当前内核执行于 EL2 时，
使用现有的直接 SECVID 切换路径，绕开失败的 ZAP PAS 调用。EL1、nVHE 和其他开发板
仍走原始路径。无需 GRUB 参数；`zap_direct_el2=0` 可关闭补丁路径用于诊断。
本包只设置原版 MSM 也支持的 `separate_gpu_kms=1`，避免卸载后遗留未知参数。

## 安装

```sh
sudo apt install dkms build-essential python3 linux-headers-$(uname -r)
sudo apt install ./rubikpi3-msm-dkms_0.1.0_arm64.deb
dkms status
modinfo -n msm
sudo reboot
```

正常情况下模块位于 `/lib/modules/$(uname -r)/updates/dkms/msm.ko.zst`。
安装、卸载均会重新生成已有内核的 initramfs。新内核由 Ubuntu 的 DKMS 和
initramfs 钩子处理；需要安装对应 headers。构建失败时请先保留并使用已验证的旧内核。
Secure Boot 强制验证环境需注册本机 DKMS 签名密钥；本次验证不覆盖该配置。

如果此前手动安装过 `updates/rubikpi3/msm.ko`，先将它移出 `/lib/modules` 备份，
并从 `/etc/modprobe.d/rubikpi3-gpu.conf` 删除旧的 `zap_direct_el2=1` 参数；
否则手工模块可能与 DKMS 冲突。保留 `separate_gpu_kms=1` 即可。
这两步只更改磁盘文件；安装成功并更新 initramfs 后再重启。

## 验证

```sh
dkms status
modinfo -n msm
sudo cat /sys/module/msm/parameters/zap_direct_el2
eglinfo -B -p surfaceless
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json vulkaninfo --summary
sudo journalctl -k -b | grep -E 'SECVID|adreno|msm|fault|hangcheck'
```

预期 OpenGL 渲染器为 `FD643`，Vulkan 为 `Turnip Adreno (TM) 643`。
还需运行实际 GPU 渲染负载；仅枚举设备成功不能证明 GPU 正常工作。
`simpledrm` 继续输出 HDMI，此包不提供原生 DPU 模式设置或休眠恢复修复。

## 卸载

```sh
sudo apt remove rubikpi3-msm-dkms
sudo reboot
```

DKMS 会恢复其备份的原版 MSM，包管理器会移除本包的 modprobe 配置并更新 initramfs。
原版驱动可能回到软件渲染；卸载不等于原版 GPU 问题已经修复。
不要在线 `rmmod msm`：这台设备此前在卸载原版 MSM 时触发过内核崩溃。
使用重启切换驱动。手工备份不会由卸载脚本自动装回。

## 从源码包构建

在有 `python3`（3.11+）、`dpkg-deb`、`patch` 的 Linux 主机上执行：

```sh
apt download linux-source-7.0.0=7.0.0-38.38
python3 build-package.py linux-source-7.0.0_7.0.0-38.38_all.deb --output dist
```

构建脚本检查固定 SHA256，提取 MSM、必要的 DRM 内部头文件和许可文本，
应用 `patches/`，生成 ARM64 源码包；无需在构建主机上编译 ARM64 模块。
完整上游源码不纳入固件仓库，生成的 deb 包包含实际 DKMS 构建所需的源码。
补丁和打包脚本为 GPL-2.0-only，上游文件保留各自的 SPDX 许可；
包内 `/usr/src/rubikpi3-msm-0.1.0/LICENSES/` 提供对应文本。

DKMS 配置及自动构建机制参见 [DKMS 3.2.2 文档](https://github.com/dkms-project/dkms/blob/v3.2.2/dkms.8.in)。

## 实机记录

打包前，等效补丁已在 Ubuntu 26.04.1 / `7.0.0-38-generic` 上通过 FD643 EGL
渲染读回、Turnip 枚举及 GNOME Shell GPU 使用验证。DKMS 包的实测结果见 [验证记录](VALIDATION.md)。
