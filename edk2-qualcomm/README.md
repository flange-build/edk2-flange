# Qualcomm 平台 EDK2 UEFI 固件

**简体中文** | [English](README.en.md)

本目录提供 Qualcomm 开发板的 UEFI 支持。代码基于上游 [edk2-platforms](https://github.com/tianocore/edk2-platforms) 中 Qualcomm Dragonwing RB3 Gen 2（`Platform/Qualcomm/RB3Gen2`）的移植，并直接复用其 `Silicon/Qualcomm` 芯片支持代码。

<a id="supported-platforms"></a>
## 支持的平台

| 开发板 | SoC | 配置名称 |
|---|---|---|
| Thundercomm RUBIK Pi 3 | QCS6490 | `rubikpi3` |

<a id="status"></a>
## 当前状态

本固件替换 `uefi_a` / `uefi_b` 分区中的原厂 Qualcomm UEFI。XBL、TrustZone、虚拟机监控程序及启动链的其他部分仍由与开发板匹配的启动固件包提供。此 ELF 用于更新 UEFI，并非完整启动固件或系统镜像。

当前已验证的官方 Ubuntu 安装器配置请先阅读 [Ubuntu Desktop 安装器](#ubuntu-desktop-installer)。

已实现的功能：

- 调试 UART 串口控制台（uart5，115200 8N1），可在倒计时期间按 ESC 或 F2 进入设置，支持英文和简体中文。
- HDMI（DSI0 → Lontium LT9611 桥）以 1920×1080@60 提供 GOP 输出，显示启动标志、控制台与设置界面，支持中文字形。RubikPi3 可保持显示链路运行并交给 Linux `simpledrm`，见[显示交接](#display-handoff)。
- UEFI 设置（语言、启动顺序、超时、Hypervisor、DSP Preload）保存在 UFS，见[设置存储](#settings)。
- UFS：为每个 LUN 提供启动项，可通过 ESP 上的 GRUB 引导 Linux。
- 加载 QUP 串行引擎固件 `qupfw_a`，供操作系统的 I2C、SPI 和蓝牙 UART 驱动使用。
- 为兼容内核提供可选的 EL2 下 ADSP/CDSP 预加载，见 [EL2 下的 DSP](#dsps-at-el2)。下述已验证的官方 Ubuntu 安装器配置应保持 **Disabled**。
- 三个 Type-A 端口支持 USB 主机模式、键盘、磁盘和 USB 启动：一个 USB 2.0 端口（SoC 的 usb_2）及两个 USB 3.0 端口（PCIe0 上的 Renesas uPD720201），见 [USB](#usb)。
- 从 XBL 留在 SMEM 中的 RAM 分区表读取完整 8 GiB DRAM，并保留固件专用内存区域。
- 向操作系统传递 RUBIK Pi 3 主线设备树。
- SMBIOS，包括设置界面中显示的内存记录。

当前板级覆盖已于 2026-10-10 验证：BOOT.MXF.1.0.c1-00430 启动固件、Hypervisor **EL2**、DSP Preload **Disabled**、官方 Ubuntu Desktop 26.04.1 ARM64 安装器（内核 `7.0.0-30-generic`）。原始 GRUB 启动项无需额外参数即可进入桌面；已检查 UFS、安装 U 盘、GNOME 与安装器进程，并确认 HDMI 实际输出。

早期项目测试使用过 flange 厂商镜像（Thundercomm 6.6.90，EL1）及 flange 主线镜像（Linux 7.0.2，EL2 并预加载 DSP）。这些结果早于当前安装器专用设备树覆盖，不能据此认定本配置下所有外设仍然可用。

尚未支持：UEFI 网络（以太网口是连接 USB 3.0 控制器的 ASIX AX88179）、UEFI 中的 USB-C、PCIe1（M.2 插槽）、1080p60 之外的显示模式、USB-C DisplayPort，以及需要 ACPI 表的 Windows（研究记录见 [docs/windows](docs/windows/)）。操作系统在运行时写入的变量不会持久化，见[设置存储](#settings)。

<a id="ubuntu-desktop-installer"></a>
## Ubuntu Desktop 安装器

1. 按[下文步骤](#flashing)刷写当前 UEFI 镜像。
2. 进入 **设备管理器 → 平台配置（Device Manager → Platform Configuration）**，设置 **Hypervisor = EL2 (KVM)**、**DSP Preload = Disabled**，按 F10 保存并重启。DSP Auto 不属于此次已验证配置。
3. 在 Boot Manager 中选择 ARM64 安装 U 盘，在 GRUB 中直接选择 **Try or Install Ubuntu**，无需编辑命令行。

使用仓库附带的设备树时，无需 `clk_ignore_unused`、`pd_ignore_unused`、`module_blacklist=qcom_ice` 或 `modprobe.blacklist=qcom_ice`。板级设备树禁用 ICE 和 GPI0/GPI1，并移除 UFS 对 ICE 的依赖，规避安装器内核中的探测故障，同时保持 UFS 可用。

此次验证覆盖 Live 桌面和安装器启动，尚未验证安装到 UFS。安装器曾损坏 LUN4 的启动分区表。Linux 会看到多个 UFS 磁盘：请按容量和分区布局确认系统安装目标，确保启动固件 LUN 不在安装计划中。修改磁盘布局前应备份启动分区和 `logfs`。`/dev/sda` 等设备名不是稳定标识。

若 DSP Auto 配置在出现 Linux 输出前就启动失败，请恢复 **DSP Preload = Disabled** 并重启。此设置让 UEFI 提前退出 Gunyah 并在 EL2 运行。针对本安装器的 DSP 预加载 / 延迟退出 Gunyah 路径仍在调查，尚未精确定位内部故障。

仅在诊断启动时，可在 GRUB 中加入 `console=ttyMSM0,115200n8 earlycon=qcom_geni,0x00994000 loglevel=8` 获取串口日志。正常启动不需要这些参数。

<a id="display-handoff"></a>
## 显示交接

RubikPi3 启用 `PcdDisplayHandoff`。`MdssDisplayDxe` 在 ExitBootServices 时保留固定的 1080p60 DPU/DSI/LT9611 链路，并请求持续保留 MDSS SMMU 旁路。帧缓冲内存被标记为保留。`DspPreloadDxe` 在系统引导器运行前，根据 GOP 填充板级 `simple-framebuffer` 模板，即使 DSP 预加载已禁用也会执行；其设备树修正协议同样处理引导器提供的模板副本。

板级 [`qcs6490-thundercomm-rubikpi3-mainline.dts`](Platform/Thundercomm/RubikPi3/DeviceTree/qcs6490-thundercomm-rubikpi3-mainline.dts) 包含基础板级设备树，并应用以下交接策略：

- 禁用原生 MDSS、DISPCC 和 LT9611 节点，保留固件正在运行的显示链路。上游桥驱动不能处理此处使用的单 B 输入配置。
- 显示供电保持开启，不依赖帧缓冲驱动的探测。帧缓冲持有 GCC 显示时钟及 HF0/HF1 MMU TBU 电源域，可在 Linux 关闭未使用资源前绑定，无需等待稳压器模块加载。
- GPIO83 保留 `output-high` 并移除 `input-disable`，避免 pinctrl 状态切换打断 LT9611 的电源使能。

不带两个 ignore-unused 参数的诊断启动中，simpledrm 约在 18 秒绑定，早于约 23 秒时正常执行的未使用时钟 / 电源域清理。随后使用未经修改的安装器启动项，也成功进入桌面并保持 HDMI 输出。

此路径使用固件帧缓冲，尚未验证原生加速显示模式设置。其他分辨率、GPU 加速与休眠恢复仍待验证。如替换 DTB，必须提供必要的交接资源；上述 Ubuntu 结果仅适用于附带的板级设备树。

<a id="what-differs-from-the-upstream-rb3-gen-2-port"></a>
## 与上游 RB3 Gen 2 移植的差异

- **内存映射：** 上游将 0x80000000–0xE0000000 描述为一整块普通 RAM，导致操作系统只看到 1.5 GiB，且其中的 Hypervisor、TrustZone、SMEM 和远程处理器区域也可能被分配。通过 EFI stub 启动的 Linux 仅依据 UEFI 建立内存映射。本项目从 SMEM 读取 DRAM 并保留专用区域，详见下文。
- **固件卷保留：** UEFI 从固件卷运行时会保留该区域。上游未设置 `PcdFdBaseAddress`，因此没有相应保留。
- **异常级别：** 上游始终请求 TrustZone 移除 Gunyah 后在 EL2 运行。厂商内核则期望像原厂固件那样作为 Gunyah 客户机启动，因此这里提供可配置选项，默认遵循 `xbl_config`。
- **SMMU：** 在 Gunyah 下，apps SMMU 会阻止来自尚未配置的客户机流的 DMA；首次 UFS 命令就可能导致 Gunyah 将系统切入崩溃转储。上游始终移除 Gunyah，因此不会遇到此问题。`SmmuDxe` 仿照 Qualcomm Hypervisor 下的 Linux，为 UFS、显示和 USB 流设置 stage 1 旁路；ExitBootServices 时恢复流表项，除非客户端请求持续交接。帧缓冲及预加载 DSP 使用持续交接路径。
- **SMBIOS：** 描述开发板、各核心及其频率、内存。
- **启动菜单：** 启动前连接所有设备，让 UFS LUN 出现在启动管理器中；倒计时与 Rockchip 平台一致，为 5 秒。
- **串口输入：** 修复轮询错误造成的每个接收字符等待 10 ms，见 `edk2-platforms-patches/`。
- **ext4：** 修复 Ext4Dxe 对块组数量向下取整，导致无法挂载小于一个块组的文件系统（4 KiB 块时为 128 MiB），如 `usb_fw` 分区，见 `edk2-platforms-patches/`。
- 设备树包含 `devicetree/mainline/upstream` 的主线源码，板级启动与显示覆盖位于 `Platform/Thundercomm/RubikPi3/DeviceTree/`。

<a id="building"></a>
## 构建

<a id="prerequisites"></a>
### 前置依赖

请参考共用的[构建依赖](../README.md#building)。Qualcomm 的 `qtestsign` 还需要 Python `cryptography` 模块：

```
sudo apt install acpica-tools binutils-aarch64-linux-gnu build-essential \
  device-tree-compiler gcc-aarch64-linux-gnu git python3 \
  python3-cryptography uuid-dev
```

初始化子模块，其中包括 [qtestsign](https://github.com/msm8916-mainline/qtestsign)：

```
git submodule update --init --recursive
```

<a id="build"></a>
### 执行构建

```
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE
```

输出 `RUBIKPI3_UEFI.elf` 位于仓库根目录。`DEBUG` 构建（默认）还会向串口输出内存映射和各驱动日志。

常规构建会给上游子模块应用补丁，可能重置并清理这些工作区。首次应用补丁后，如需保留子模块的本地修改，可运行：

```sh
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE --skip-patchsets
```

全新克隆时不要用此选项跳过必要的首次补丁应用。仓库的 GitHub Actions 矩阵目前仅构建 Rockchip 开发板；RubikPi3 单独构建和验证。

通过 `--edk2-flags` 传入构建选项，例如：

```
./edk2-qualcomm/build.sh -d rubikpi3 --edk2-flags "-D EXIT_GUNYAH=TRUE"
```

| 选项 | 默认值 | 含义 |
|---|---|---|
| `EXIT_GUNYAH` | `FALSE` | Hypervisor 设置与 `xbl_config` 均未指定异常级别时使用。`TRUE` 会移除 Gunyah，使 UEFI 和操作系统在 EL2 运行，与上游 RB3 Gen 2 行为一致。 |
| `DEFAULT_LANG` | `en-US` | 用户在 Select Language 中选择语言前的菜单默认语言，可用 `en-US` 或 `zh-Hans`。 |

<a id="exception-level"></a>
## 异常级别

XBL 始终在 Gunyah 下以 EL1 启动 UEFI。控制台初始化之前，UEFI 就会决定保留 Gunyah，还是使用与原厂 UEFI 相同的 SMC 将其移除并切换至 EL2。判断优先级如下：

1. **设备管理器 → 平台配置 → Hypervisor（虚拟机监控程序）** 设置：EL1 (Gunyah) 或 EL2 (KVM)，下次重启生效。
2. 若为 Auto（默认），则像原厂 UEFI 一样读取 `xbl_config` 的 `OsConfigTableSelection`。`xbl_config.elf` 为 1（Gunyah，EL1）；flange 主线产品刷入的 `xbl_config_kvm.elf` 为 2（KVM，EL2）。XBL 将对应设备树留在内存，并在 0x146AA000 的共享 IMEM cookie 中存储地址（+0x58）和大小（+0x60）。
3. 若仍无法读取，则使用 `EXIT_GUNYAH` 构建选项。

目标为 EL2 时，可以立即退出 Gunyah；若需预加载 DSP，则延迟到系统引导器的 ExitBootServices 成功后退出，与原厂 UEFI 的调用时机一致，见 [EL2 下的 DSP](#dsps-at-el2)。Gunyah 每次启动只接受一次这类调用，因此延迟退出的启动不会像 EL1 启动那样在早期进行确认调用。

即使在 `RELEASE` 构建中，控制台也会输出选择结果和原因，例如：

```
QCS6490: hypervisor: setting Auto, xbl_config KVM -> EL2 (KVM)
QCS6490: hypervisor: Gunyah stays until ExitBootServices (DSP preload)
```

平台配置页面也显示相同结果。TrustZone 拒绝调用时，原厂 UEFI 会停止，而本固件输出警告后继续在 EL1 运行。

厂商内核的远程处理器（ADSP、CDSP、视频）依赖 EL1。已测试的主线内核在 EL2 使用 KVM。内核能否接管预加载 DSP 取决于配置；上述官方 Ubuntu 安装器请使用 DSP Disabled。

<a id="dsps-at-el2"></a>
## EL2 下的 DSP

没有 Gunyah 时，Linux 无法在此板上启动 ADSP 和 CDSP：它会向 TrustZone 请求资源表，但此版本 TrustZone 不提供该表。Linux 7.0 可接管由启动固件启动的 DSP，在探测时检查 SMP2P 中的 ready 和 handover 状态，与 Radxa Dragon Q6A 的处理方式类似。

启用后，`DspPreloadDxe` 在 ReadyToBoot 阶段仿照 Linux `qcom_q6v5_pas` 启动 DSP：通过 Ext4Dxe 从操作系统根文件系统的 `/usr/lib/firmware` 读取固件（分区由 `PcdDspFirmwarePartition` 指定），发送 AOP `load_state` 消息，对供电和 CDSP 内存路径进行代理投票，创建 Linux SMP2P 驱动所需条目，并调用 TrustZone PAS，等待 ready 和 handover。

TrustZone 只会实际运行它为 Gunyah 客户机启动的 DSP：在 EL2 下 PAS 调用虽然成功，DSP 却不会运行。因此，预加载 DSP 后在 EL2 启动系统的路径会将 Gunyah 保留到 ExitBootServices，与原厂 UEFI 一致。`GunyahExitDxe` 包装 ExitBootServices，待其成功后退出 Gunyah，使用 UEFI 的地址转换表在 EL2 继续执行，再返回系统引导器。

Gunyah 会清除未保留在旁路模式的 SMMU 流表项，因此 `SmmuDxe` 随后重新配置 DSP 流，让 Linux 在旁路状态下接管；同时通过 `EFI_DT_FIXUP_PROTOCOL` 移除 GRUB 加载设备树中 remoteproc 节点的 `iommus`，避免 Linux 将运行中的 DSP 放入空的 SMMU 域。

**DSP Preload（预加载 DSP）** 可选 Auto（系统在 EL2 时启用）、Disabled 或 Always（EL1 也启用），并可分别选择 DSP。SEC 也读取此设置，因此下次重启生效。Disabled 恢复启动早期退出 Gunyah 的路径。`PcdGunyahLateExit` 控制退出时机：0 始终早期退出；1（默认）在预加载 DSP 时延迟至 ExitBootServices；2 始终在 ExitBootServices 退出。

控制台会输出各步骤，例如：

```
DspPreload: adsp running after 146 ms
GunyahExit: leaving Gunyah: srtm
HandOverNow: adsp stream 0x1800 mask 0x0: entry 0, SMR 0x80001800, S2CR 0x100FF, kept for the OS
DspPreload: after the Gunyah exit: adsp SMP2P 0x6 (running)
```

`srtm` 分别表示发起调用、返回 EL2、装载转换表、开启 MMU。日志停在哪个字符之后，可用于定位切换故障阶段。

<a id="usb"></a>
## USB

| 端口 | 控制器 | 驱动 |
|---|---|---|
| USB 2.0 Type-A | usb_2，SoC 的次级 DWC3（仅 USB 2.0） | `Dwc3HostDxe` |
| 两个 USB 3.0 Type-A、以太网 | PCIe0 上的 Renesas uPD720201 xHCI | `Qcs6490PciHostBridgeLib`、`RenesasXhciFwDxe` |
| USB-C | usb_1，主 DWC3 | 无：用于 EDL 和 adb 的设备模式 |

UEFI 之前的启动阶段不会初始化这两个主机控制器，因此这里参考 Linux 7.0：`Dwc3HostDxe` 为 usb_2 及其 HS PHY 上电，设置主机模式；`Qcs6490PciHostBridgeLib` 为 PCIe0 相关电源、QMP PHY 和根复合体上电，在 PciBusDxe 枚举前完成 Gen2 x1 链路训练。随后由 XhciDxe 驱动两者。

uPD720201 没有 EEPROM。Linux 每次启动都从 `usb_fw` 分区（UFS LUN 3，ext4）的 `renesas_usb_fw.mem` 下载固件。`RenesasXhciFwDxe` 在 XhciDxe 接管前通过 Ext4Dxe 完成相同步骤；固件不包含在本仓库中。芯片持续供电时会保留固件，因此 Linux 检测到固件已运行后，会跳过下载及其前面的五秒等待。

ExitBootServices 时，PCIe0 关闭链路训练、拉低 PERST#，但保持供电。Linux 探测 PCIe0 前数秒就会关闭尚未使用的时钟，其中包括链路参考时钟；若让 uPD720201 一直运行并保持活动链路，后续只能以 2.5 GT/s 建链。保持复位则可让 Linux 重新以 5 GT/s 建链。

与 UFS 一样，所有 USB DMA 均限制在 4 GiB 以下：Gunyah 下 usb_2 对 DRAM 顶部缓冲区的 DMA 曾超时。UEFI 运行期间，`SmmuDxe` 放行两个控制器的流；XhciDxe 在 ExitBootServices 时停止控制器。

<a id="settings"></a>
## 设置存储

UEFI 变量及所有设置保存在 UFS LUN 4 的 `logfs` 分区，该分区原本用于原厂 UEFI 日志。只更新 `uefi_a` / `uefi_b` 会保留设置；整盘恢复、重写分区表或安装器操作可能擦除它并重置设置，进行这些操作前请备份。

- SEC 在选择异常级别前，临时放行 UFS 的 SMMU DMA，并借用 XBL 留下的 UFS 控制器状态，将变量存储读入 0xA0000000。DXE 阶段会重新初始化控制器。
- `NvStoreFvbDxe` 将这块内存交给标准变量驱动，UFS 就绪后将每次修改写回分区。
- 首次启动会格式化变量存储。如果 SEC 无法读取分区，该次启动仅在内存中保存变量，不修改分区；控制台和平台配置页面会说明原因。
- 操作系统在运行时写入的变量不回写磁盘。固件通过 `EFI_RT_PROPERTIES_TABLE` 声明这一限制，因此 Linux 将 `efivarfs` 保持为只读。

每块板可通过 `PcdNvStoreUfsLun` 和 `PcdNvStorePartitionName` 指定存储分区。

<a id="flashing"></a>
## 刷写

使用开发板启动固件包中匹配的 `prog_firehose_ddr.elf`，并通过支持 EDL 的 USB 连接操作。开发板必须接受 qtestsign 测试签名 ELF；本构建不提供生产签名密钥。下载器及其他启动固件不包含在此镜像中。

进入 EDL 后，确认主机识别到 Qualcomm **05c6:9008**。**900e** 是另一种 RamDump 模式。写入前先读取实际 GPT：

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS printgpt --lun 4
```

已测试布局中，LUN4 的 `uefi_a` 和 `uefi_b` 各为 5 MiB。请按分区名操作，避免硬编码扇区偏移。更新前将两个槽位及设置存储备份到新的文件：

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_a uefi_a-before.bin --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_b uefi_b-before.bin --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part logfs logfs-before.bin --lun 4
```

对于上述 A/B 布局，逐个写入并读回校验：

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS write-part uefi_a RUBIKPI3_UEFI.elf --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_a uefi_a-after.bin --lun 4
cmp -n "$(stat -c%s RUBIKPI3_UEFI.elf)" RUBIKPI3_UEFI.elf uefi_a-after.bin

edl-ng --loader prog_firehose_ddr.elf --memory UFS write-part uefi_b RUBIKPI3_UEFI.elf --lun 4
edl-ng --loader prog_firehose_ddr.elf --memory UFS read-part uefi_b uefi_b-after.bin --lun 4
cmp -n "$(stat -c%s RUBIKPI3_UEFI.elf)" RUBIKPI3_UEFI.elf uefi_b-after.bin
```

只有命令执行和比较均成功后，才继续重启：

```sh
edl-ng --loader prog_firehose_ddr.elf --memory UFS reset --delay 1
```

回退时，将各槽位对应的备份分区镜像写回。除非希望重置设置，否则保留 `logfs`。若 LUN4 GPT 或其他启动固件分区缺失，仅更新 EFI 无法修复启动链：应先恢复匹配的板级固件布局，再替换 UEFI。完整固件恢复与普通 UEFI 更新是不同操作。

<a id="memory-map"></a>
## 内存映射

XBL 只允许将 UEFI 镜像加载到 0x9FB00000–0xA0A00000；原厂 UEFI 使用 0x9FB00000–0xA0000000。本镜像从 0x9FC00000 加载：FD 占用 0x9FC00000–0x9FF00000，SEC 栈从 0x9FF00000 开始，变量存储及 SEC 状态页占用 0xA0000000–0xA0091000，PEI 和早期 DXE 在 0xDC000000–0xE0000000 运行。

DRAM 信息读取自 SMEM 的 RAM 分区表（条目 402）。若无法读取，固件回退为从 0x80000000 开始的 2 GiB，并输出提示。以下专用区域被保留，且不在 UEFI 中映射：

| 地址范围 | 所属组件 |
|---|---|
| 0x80000000-0x83600000 | Hypervisor、XBL、AOP、command DB、SMEM、CPUCP、WLAN 固件、CDSP 安全堆 |
| 0x84300000-0x9AE00000 | 远程处理器：camera、WPSS、ADSP、CDSP、SPSS、video、CVP、IPA、GPU、modem |
| 0x9CB80000-0x9D380000 | ADSP RPC 远程堆 |
| 0xC0000000-0xC3400000 | TrustZone：统计、标签、QTEE、可信应用 |
| 0xD0600000-0xD0700000 | 调试 VM |
| 0xE0000000-0xE0F00000 | DBI 转储 |

此表取原厂 Qualcomm UEFI 内存映射与 QCS6490 主线 / 厂商设备树保留区域的并集，并去除原厂启动画面区域。实现位于 `Silicon/Qualcomm/QCS6490/Library/Qcs6490Lib/Qcs6490Mem.c`。

<a id="layout"></a>
## 目录结构

```
edk2-qualcomm/
├── build.sh                     构建入口
├── configs/                     每块开发板一个配置文件
├── docs/windows/                Windows 11 on Arm 研究记录（尚未实现）
├── edk2-platforms-patches/      上游 edk2-platforms 补丁
├── misc/qtestsign/              ELF 哈希段与测试签名（子模块）
├── Platform/Thundercomm/RubikPi3/
│   ├── RubikPi3.dsc             板级 PCD 与设备树
│   ├── RubikPi3.Modules.fdf.inc
│   ├── Drivers/BoardDxe/        LT9611 控制信号
│   └── DeviceTree/
│       ├── Mainline.inf
│       └── qcs6490-thundercomm-rubikpi3-mainline.dts
└── Silicon/Qualcomm/QCS6490/    QCS6490 开发板共用代码
    ├── QCS6490.dec
    ├── QCS6490.dsc.inc
    ├── QCS6490.fdf
    ├── Library/
    │   ├── Qcs6490Lib/          ArmPlatformLib：异常级别选择与切换、
    │   │                        早期 UFS 变量读取、内存映射
    │   ├── Qcs6490NvStatusLib/  向 DXE 提供 SEC 阶段决策
    │   ├── Qcs6490RpmhLib/      通过 apps RSC 进行 RPMh 投票
    │   ├── Qcs6490GccLib/       GCC 电源域、时钟与复位
    │   ├── Qcs6490TlmmLib/      TLMM 引脚
    │   ├── Qcs6490PciHostBridgeLib/  PCIe0 初始化与根桥
    │   ├── Qcs6490PciSegmentLib/     PCIe0 配置空间
    │   ├── MemoryInitPeiLib/    MMU 配置
    │   └── OemMiscLib/          SMBIOS
    └── Drivers/
        ├── SmmuDxe/             Gunyah 下 UFS、显示与 USB DMA 的 SMMU 配置，
        │                        将显示与 DSP 流交接给操作系统
        ├── NvStoreFvbDxe/       变量存储 FVB 与 UFS 回写
        ├── PlatformConfigDxe/   平台配置页面（Hypervisor、DSP 预加载）
        ├── MdssDisplayDxe/      HDMI：DPU、DSI、LT9611、GOP
        ├── QupFwDxe/            为操作系统加载 QUP 串行引擎固件
        ├── DspPreloadDxe/       可选 DSP 预加载与 GOP 设备树修正
        ├── GunyahExitDxe/       在 ExitBootServices 时退出 Gunyah
        ├── Dwc3HostDxe/         USB 2.0 端口（usb_2）主机模式
        ├── RenesasXhciFwDxe/    从 usb_fw 分区加载 uPD720201 固件
        └── SmbiosMemoryDxe/     SMBIOS 内存记录
```

启动标志（`edk2-common/Drivers/LogoDxe`）和中文字体（`edk2-rockchip/Silicon/Rockchip/Drivers/CjkFontDxe`）与 Rockchip 平台共用。
