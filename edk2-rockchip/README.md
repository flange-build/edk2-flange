# Rockchip RK3588 平台 EDK2 UEFI 固件

**简体中文** | [English](README.en.md)

本指南介绍 **edk2-flange** 的 Rockchip RK3588 平台。项目概览和 Qualcomm 支持见[项目首页](../README.md)。

下列平台分级和系统兼容性表继承自 RK3588 移植，不能视为本分支对每块开发板重新完成的硬件验证。所有构建命令均在仓库根目录执行。

固件提供类似 PC 的标准化启动体验，支持 Windows、Linux、BSD 和 VMware ESXi 等多种操作系统。

![EDK2 主界面](../images/edk2-frontpage.png)

<a id="supported-platforms"></a>
# 支持的平台

支持程度分为白金（Platinum）和青铜（Bronze）两档。

白金设备具有较好的整体支持，评估因素包括：

- 设备树和外设兼容主线 Linux。【必需】
- 厂商积极参与硬件支持。
- 若提供以太网口，使用 Realtek PCIe 网卡或集成 GMAC。【必需】
- 配有用于独立存放固件的 SPI NOR。【优先】

青铜设备可能缺少上述必需功能，厂商或社区参与度较低，或验证不足而影响实际功能。随着设备支持状况变化，分级也可能调整。

<a id="platinum"></a>
## 白金

- [Radxa ROCK 5B](https://radxa.com/products/rock5/5b/)
- [Radxa ROCK 5A](https://radxa.com/products/rock5/5a/)
- [Radxa ROCK 5 ITX](https://radxa.com/products/rock5/5itx/)
- [Orange Pi 5](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5.html)
- [Orange Pi 5 Plus](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-plus.html)
- [Khadas Edge2](https://www.khadas.com/edge2)
- [BuzzTV P6](https://buzztvglobal.com/products/powerstation-6)
- [FriendlyELEC NanoPC T6](https://wiki.friendlyelec.com/wiki/index.php/NanoPC-T6)
- [FriendlyELEC NanoPi R6C](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R6C)
- [FriendlyELEC NanoPi R6S](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R6S)
- [FriendlyELEC NanoPC CM3588-NAS](https://wiki.friendlyelec.com/wiki/index.php/CM3588_NAS_Kit)
- [ameriDroid Indiedroid Nova](https://indiedroid.us)

<a id="bronze"></a>
## 青铜

- [Radxa ROCK 5B+](https://radxa.com/products/rock5/5bp)
- [Fydetab Duo](https://fydetabduo.com/)
- [Firefly AIO-3588Q](https://en.t-firefly.com/product/industry/aio3588q)
- [Firefly ITX-3588J](https://en.t-firefly.com/product/industry/itx3588j)
- [Firefly ROC-RK3588S-PC](https://en.t-firefly.com/product/industry/rocrk3588spc) / [StationPC Station M3](https://www.stationpc.com/product/stationm3)
- [Mekotronics R58X](https://www.mekotronics.com/h-pd-75.html)
- [Mekotronics R58 Mini](https://www.mekotronics.com/h-pd-76.html)
- [Mixtile Blade 3](https://www.mixtile.com/blade-3)
- [FriendlyELEC NanoPi M6](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_M6)
- [Hinlink H88K](http://www.hinlink.com)

<a id="supported-oses"></a>
# 支持的操作系统

<a id="in-acpi-mode"></a>
## ACPI 模式

| 操作系统 | 版本 | 已测试 / 支持的硬件 | 备注 |
| --- | --- | --- | --- |
| Windows | 11 | [支持状态](https://github.com/worproject/Rockchip-Windows-Drivers#hardware-support-status) | |
| NetBSD | 10 | 显示、UART、USB、PCIe（含 NVMe）、SATA、eMMC、GMAC 以太网 | |
| VMware ESXi Arm Fling | >= 1.12 | 显示、USB | PCIe 设备会导致启动挂起，需要在设置中禁用或保持端口为空。GMAC 以太网可被识别，但无法工作。 |
| Linux | 已测试 Ubuntu 22.04，内核 5.15.0-75-generic | 显示、UART、USB、PCIe（含 NVMe 和以太网）、SATA | 如需完整硬件支持，请使用支持 RK3588 的内核并切换至设备树模式。 |

> [!NOTE]
> 此移植的 ACPI 开发和测试以 Windows 为目标，目前没有继续完善其他系统 ACPI 功能的计划。对于 Linux 等支持设备树的系统，建议使用设备树模式。

<a id="in-device-tree-mode"></a>
## 设备树模式

<a id="vendor-compatibility-mode"></a>
### 厂商兼容模式

| 操作系统 | 版本 | 已测试 / 支持的硬件 | 备注 |
| --- | --- | --- | --- |
| Rockchip SDK Linux | 内核 5.10/6.1；已测试 [Armbian rk3588-live-iso](https://github.com/amazingfate/rk3588-live-iso) | 取决于平台，大多数外设可用。 | 使用其他内核时请参阅[设备树配置](#device-tree-configuration)。 |

<a id="mainline-compatibility-mode"></a>
### 主线兼容模式

| 操作系统 | 版本 | 已测试 / 支持的硬件 | 备注 |
| --- | --- | --- | --- |
| 通用上游 Linux | 内核 6.10 或更新；已测试 Ubuntu 24.10、Fedora Workstation 41 和 Rawhide | 取决于平台和内核版本，见 [Collabora 的 RK3588 主线支持状态](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md)。 | 6.15 之前的内核缺少显示输出，临时解决方法见[设备树配置](#device-tree-configuration)。 |

> [!NOTE]
> 主线支持仅适用于[白金](#platinum)平台。

<a id="supported-peripherals-in-uefi"></a>
# UEFI 中支持的外设

> [!NOTE]
> 除非特别注明，以下内容适用于所有平台，仅列出与固件本身有关的设备，不代表操作系统支持情况。

| 设备 | 状态 | 备注 |
| --- | --- | --- |
| USB 3 / 2.0 / 1.1 | 🟢 可用 | 仅主机模式；Type-C 连接 USB 3 设备时只有一个插入方向可用。 |
| PCIe 3.0 / 2.1 | 🟢 可用 | |
| SATA | 🟢 可用 | |
| SD/eMMC | 🟢 可用 | |
| HDMI 输出 | 🟢 可用 | |
| DisplayPort 输出（USB-C） | 🟡 部分支持 | 无热插拔检测和 EDID；Type-C 仅一个插入方向可用，部分显示器可能始终无法工作。 |
| eDP 输出 | 🟡 部分支持 | 默认禁用，需要按平台和面板手动配置。 |
| DSI 输出 | 🟢 可用 | 仅在 Fydetab Duo 上启用；其他平台和面板需要手动配置。 |
| GMAC 以太网 | 🟢 可用 | |
| Realtek PCIe 以太网 | 🟢 可用 | 部分平台未设置 MAC 地址，可能导致网络不可用。 |
| 低速接口（GPIO/UART/I2C/SPI/PWM） | 🟢 可用 | UART2 控制台波特率为 1500000。 |
| SPI NOR Flash | 🟢 可用 | |
| HYM8563 实时时钟 | 🟢 可用 | |
| RNG | 🟢 可用 | |
| 散热风扇 | 🟢 可用 | 大多数平台支持。优先使用风扇接口，否则可通过 GPIO 接针连接三线 PWM 风扇（**不要**接两线风扇）：Orange Pi 5 为 `GPIO4_B2`，Indiedroid Nova 为 `GPIO4_B4`。 |
| 状态 LED | 🟢 可用 | |
| 稳压器（RK806/RK860） | 🟢 可用 | |
| FUSB302 USB Type-C 控制器 | 🔴 不可用 | 用于 PD 协商和连接器方向切换。 |

<a id="getting-started"></a>
# 开始使用

<a id="1-requirements"></a>
## 1. 准备条件

- 一块[受支持的开发板](#supported-platforms)。
- 固件存储介质：SPI NOR（部分板载）、SD 卡或 eMMC。
- 至少能提供 15 W 的可靠电源；外设较多时可能需要更高功率。Mixtile Blade 3 必须使用**高于 5 V** 的固定电压供电；仅输入 5 V 时无法为外部外设供电，固件不支持 USB-PD 协商。
- HDMI（推荐）或 DisplayPort（USB-C）显示器。
- 可选：支持 1500000 波特率的 UART 转接器（如 USB CH340、CP2104），用于无显示器操作或调试。

<a id="2-download-the-firmware-image"></a>
## 2. 获取固件镜像

edk2-flange 请按[项目构建说明](../README.md#building)从源码构建目标开发板。[上游 RK3588 发布版本](https://github.com/edk2-porting/edk2-rk3588/releases)属于独立发行，不包含本分支的 Qualcomm 支持。

如果平台尚未受支持，**不建议**使用其他设备的镜像。即使硬件相似，供电电压配置也可能不同，存在损坏开发板的风险，外设也可能无法工作。

<a id="3-flash-the-firmware"></a>
## 3. 刷写固件

UEFI 可写入 SPI NOR、SD 卡或 eMMC：

- 可移除的 SD 或 eMMC 最简单，可直接使用 balenaEtcher、RPi Imager 或 dd。
- SPI NOR 或焊接式 eMMC 可参阅 [Radxa 刷写说明](https://docs.radxa.com/en/rock5/lowlevel-development/bootloader_spi_flash)。可在板端 Linux 下刷写，也可在另一台电脑使用 RKDevTool；后者需要设备进入 MaskROM，具体方法请查阅对应厂商文档。

**警告：这些操作会擦除存储设备上的数据，请先备份。**

若希望 UEFI 和操作系统共用一张 SD 卡或 eMMC，请先刷 UEFI，再创建其他分区，不要修改第一个保留分区。此布局下的后续更新方法见[更新固件](#updating-the-firmware)。

有 SPI NOR 时建议优先使用，可将其他存储留给系统。SD/eMMC 还会限制操作系统运行期间固件访问自身变量存储的能力；该能力主要用于系统安装器创建启动菜单项，并非启动所必需。

<a id="4-connect-peripherals-and-power-on-the-device"></a>
## 4. 连接外设并上电

正确刷写后，状态 LED（若有）应开始闪烁，随后显示器显示平台启动标志及底部进度条。

此时按 <kbd>Esc</kbd> 进入设置，按 <kbd>F1</kbd> 启动 UEFI Shell；若存储上有 UEFI 引导器或应用，不进行操作时默认自动运行。

请查阅[支持的操作系统](#supported-oses)、[UEFI 外设支持](#supported-peripherals-in-uefi)及下方配置说明，按所用系统调整设置。遇到问题可参阅[故障排查](#troubleshooting)。

<a id="configuration-settings"></a>
# 配置设置

UEFI 提供 CPU 频率、M.2 端口的 PCIe/SATA 选择、风扇控制等选项，可在 `Device Manager` → `Rockchip Platform Configuration` 中查看和修改。菜单周围提供帮助和导航说明。

<a id="language"></a>
## 语言

菜单支持英文和简体中文，可在首页的 `Select Language` 中切换，并跨重启保存选择。

串口显示中文需要支持 UTF-8 的终端（如 `picocom`、`minicom -c on`、`screen` 或设置为 UTF-8 的 PuTTY），以及包含 CJK 字形的字体。

<a id="tips"></a>
## 使用建议

<a id="boot-time-optimization"></a>
### 缩短启动时间

- 对不用的 M.2/PCIe 插槽，在 `Device Manager` → `Rockchip Platform Configuration` → `PCIe/SATA/USB Combo PIPE PHY` 中将对应 PHY 设置为 `Unconnected`；`PCI Express 3.0` 的 `Support State` 也可设为 `Disabled`。
- 在 `Boot Maintenance Manager` 中缩短自动启动等待时间。
- 不使用网络启动时，在 `Device Manager` → `Network Stack Configuration` 中取消勾选 `Network Stack`。
- 若无需在固件中热插拔显示器或使用 DisplayPort，可将 `Device Manager` → `Rockchip Platform Configuration` → `Display` → `Force Output` 设为 `Disabled`。没有连接显示器时将跳过显示初始化。
- 固件默认连接所有启动设备，以避免兼容性问题，额外耗时通常很小，建议保留默认值。如确需修改，可使用 `Boot Maintenance Manager` → `Boot Discovery Policy`。

<a id="linux-boot"></a>
### Linux 启动

若某些发行版启动时报 Synchronous Exception，可进入 `Device Manager` → `EFI Memory Attribute Protocol`，取消勾选 `Enable Protocol`。

<a id="device-tree-configuration"></a>
## 设备树配置

为获得更完整的 Linux 支持，建议在 `Device Manager` → `Rockchip Platform Configuration` → `ACPI / Device Tree` 中将 `Config Table Mode` 设置为 `Device Tree`。

固件提供两种兼容模式：

- `Vendor`：仅兼容 Rockchip SDK Linux 5.10/6.1 内核。
- `Mainline`：兼容通用上游 Linux 6.10 或更新内核。该模式仍在开发，可能缺少部分功能，建议使用较新的内核和固件以获得改进的设备支持。

[白金](#platinum)平台默认启用 `Mainline`，[青铜](#bronze)平台默认回退到 `Vendor`。

> [!TIP]
> `Mainline` 模式搭配 6.15 之前的通用 Linux 内核时，HDMI 无法正常输出。可在 `Device Manager` → `Rockchip Platform Configuration` → `ACPI / Device Tree` 中启用 `Force UEFI GOP Display`，使用 UEFI 初始化的显示输出；此模式不支持 GPU 加速。

<a id="custom-device-tree-blob-dtb-override-and-overlays"></a>
### 自定义 DTB 与设备树覆盖层

固件自带 DTB 过旧、与内核不匹配或需要测试时，可提供自定义 DTB 和覆盖层。在上述 `ACPI / Device Tree` 菜单中将 `Support DTB override & overlays` 设为 `Enabled`，固件便会在所选启动设备的所有支持的文件系统 / 分区（FAT、ext4）中查找。

**注意：** 下列路径均相对于分区根目录，不能再放进其他子目录。基础 DTB 和所有覆盖层必须在同一个分区，不能跨分区组合。

基础 DTB 可放在以下位置，文件名必须为 `<PLATFORM-DT-NAME>.dtb`：

- `\dtb`
- `\dtb\base`
- `\dtb\rockchip`：Fedora 镜像的第二个 ext4 启动分区使用此路径存放内核 DTB。

覆盖层扩展名必须为 `.dtbo`，可放在：

- `\dtb\overlays`：与平台无关，优先应用。
- `\dtb\overlays\<PLATFORM-DT-NAME>`：仅应用于指定平台。

也可通过同一菜单中的 `Preferred Base DTB Path` 和 `Preferred Overlays Path` 指定自定义路径。

`<PLATFORM-DT-NAME>` 可取以下值：

| 名称                                    | 平台                      |
| --------------------------------------- | ----------------------------- |
| `rk3588-rock-5b`                        | ROCK 5B                       |
| `rk3588-rock-5bp`                       | ROCK 5B+                      |
| `rk3588s-rock-5a`                       | ROCK 5A                       |
| `rk3588-rock-5-itx`                     | ROCK 5 ITX                    |
| `rk3588s-orangepi-5`                    | Orange Pi 5                   |
| `rk3588-orangepi-5-plus`                | Orange Pi 5 Plus              |
| `rk3588s-9tripod-linux`                 | Indiedroid Nova               |
| `rk3588s-fydetab-duo`                   | Fydetab Duo                   |
| `rk3588-buzztv-p6`                      | PowerStation 6                |
| `aio-3588q`                             | Firefly AIO-3588Q             |
| `itx-3588j`                             | Firefly ITX-3588J             |
| `roc-rk3588s-pc`                        | ROC-RK3588S-PC / Station M3   |
| `rk3588-blueberry-edge-v12-linux`       | R58X (v1.2)                   |
| `rk3588-blueberry-minipc-linux`         | R58 Mini                      |
| `rk3588s-khadas-edge2`                  | Edge2                         |
| `rk3588-blade3-v101-linux`              | Blade 3                       |
| `rk3588-nanopc-t6`                      | NanoPC T6                     |
| `rk3588-nanopc-cm3588-nas`              | NanoPC CM3588-NAS             |
| `rk3588s-nanopi-r6c`                    | NanoPi R6C                    |
| `rk3588s-nanopi-r6s`                    | NanoPi R6S                    |
| `rk3588s-nanopi-m6`                     | NanoPi M6                     |
| `rk3588-hinlink-h88k`                   | H88K                          |

补充说明：

- 固件会按用户设置修正 DTB，如 PCIe/SATA/USB 选择，因此不必额外添加 SATA 覆盖层。通过 GRUB `devicetree` 等其他方式替换 DTB 时，不会执行这些修正。
- 未提供基础 DTB 时，覆盖层应用于固件自带 DTB。
- 任一覆盖层应用失败（如与基础 DTB 不兼容）时，其他覆盖层也会被丢弃。
- 自定义基础 DTB 无效时，改为向系统传递固件自带 DTB。
- 此过程记录到[串口控制台](#advanced-troubleshooting)，查看潜在错误需要使用串口日志。

<a id="updating-the-firmware"></a>
# 更新固件

若存储仅用于 UEFI，可按[开始使用](#getting-started)中的方法将新镜像完整刷入。

若与操作系统共用存储且存在其他分区，只需更新镜像的一部分：

```bash
dd if=FIRMWARE.img of=DESTINATION bs=512 skip=64 seek=64 conv=notrunc
```

`FIRMWARE.img` 是匹配平台的固件镜像，例如 `edge2_UEFI_Release_v0.8.img`；`DESTINATION` 是待更新的存储设备，例如 `/dev/sdb`。

该命令跳过 GPT，从偏移 0x8000（64 块 × 512 字节）开始复制到镜像末尾。详见[闪存布局](#flash-layout)。

<a id="flash-spi-nor-from-the-uefi-shell"></a>
## 通过 UEFI Shell 刷写 SPI NOR

1. 将固件镜像复制到存储设备的 FAT32 分区，并连接开发板。
2. 启动 UEFI Shell：开机时按 <kbd>F1</kbd>，或选择 `Boot Manager` → `UEFI Shell`。
3. 使用 `map` 查看已挂载文件系统（如 `fs0:`、`fs1:`），输入对应名称并按 <kbd>Enter</kbd> 进入。可用 `ls fsX:` 查看内容，`X` 替换为实际编号。
4. 运行 `sf updatefile FIRMWARE.img 0x0`，等待更新完成。
5. 重启设备。

<a id="troubleshooting"></a>
# 故障排查

> [!IMPORTANT]
> 首先确保设备只能加载目标 UEFI 固件。
>
> **SPI NOR、SD 和 eMMC 上都不应残留 U-Boot，否则它可能优先启动并引发问题。**

以下为基础排查方法；若仍不能解决，请参阅[高级排查](#advanced-troubleshooting)。

<a id="meaning-of-the-status-led"></a>
## 状态 LED 的含义

有活动指示灯的平台通过不同闪烁模式表示系统状态：

1. 刚上电时快速闪烁，表示正在初始化固件。
2. 初始化完成后（通常不到 5 秒），约每两秒短闪一次，表示固件就绪，等待用户操作或自动启动倒计时。此时显示输出也应已开启。
3. 固件开始引导系统并即将退出时，LED 停止闪烁。

若上电后完全不亮，说明固件未能加载；若闪几次后始终保持亮或灭，且未到第 3 步，则可能崩溃或挂起。

<a id="recovery"></a>
## 恢复模式

如果不方便按物理 MaskROM 按键，可从 Boot Manager 选择对应启动项，或在启动画面按 <kbd>F4</kbd> 进入 MaskROM。

上电时按住 Recovery（或音量加）按键也可进入该模式。

<a id="common-issues"></a>
## 常见问题

<a id="nothing-shows-up-on-the-screen"></a>
### 屏幕没有画面

首先确认镜像确实匹配开发板且已正确刷写，这通常是最常见原因。如果固件已正常加载：

- 显示器必须至少支持 640×480@60 Hz。
- 尝试不连接显示器启动，几秒后（状态 LED 模式变化时）再接入，以强制使用最低支持分辨率。随后可在 `Device Manager` → `Rockchip Platform Configuration` → `Display` 中提高分辨率。
- 使用 USB-C 转 DisplayPort 时只有一个插入方向可用，请尝试翻转插头。

仍无画面时，可通过[串口控制台](#advanced-troubleshooting)与 UEFI 交互。

<a id="configuration-settings-do-not-get-saved"></a>
### 设置无法保存

当多个设备（SPI NOR、eMMC、SD）同时存有固件时，可能出现此问题。该布局不受支持，因为 UEFI 无法准确判断自身启动设备。请移除或擦除其他存储上的固件。

<a id="usb-3-devices-do-not-work"></a>
### USB 3 设备无法工作

- 尝试其他端口。
- Type-C 的 USB 3.0 仅一个插入方向可用，请翻转插头测试。
- 检查电源与线材质量。

<a id="networking-does-not-work"></a>
### 网络无法工作

UEFI 仅支持集成千兆以太网（GMAC）以及 Realtek PCIe 和 USB 网络控制器。

部分 Realtek 网卡出厂时没有配置 MAC 地址，在 UEFI 中显示全零，可能无法获取 IP。可手动写入 MAC 地址：

1. 启动 Linux 并打开终端。以下命令适用于使用 legacy 内核的 Armbian。
2. 安装对应内核头文件：

   ```bash
   sudo apt install -y linux-headers-legacy-rk35xx
   ```

3. 克隆 Realtek PGTool 并构建驱动：

   ```bash
   git clone https://github.com/redchenjs/rtnicpg
   cd rtnicpg
   make
   ```

4. 卸载 Realtek 模块并加载新驱动：

   ```bash
   sudo rmmod pgdrv
   sudo ./pgload.sh
   ```

   确保除新的 `pgdrv` 外没有其他 Realtek 模块仍在加载。若 `r8125` 内建于内核，可能需要在 GRUB 中加入 `initcall_blacklist=rtl8125_init_module` 后重启。

5. 将 MAC 地址写入 eFuse。单网卡：

   ```bash
   sudo ./rtnicpg-aarch64-linux-gnu /efuse /nodeid 00E04C001234
   ```

   两个或更多网卡：

   ```bash
   sudo ./rtnicpg-aarch64-linux-gnu /efuse /# 1 /nodeid 00E04C001234
   sudo ./rtnicpg-aarch64-linux-gnu /efuse /# 2 /nodeid 00E04C001235
   ```

   `00E04C001234` 仅为示例，可使用 [MAC 地址生成器](https://www.macvendorlookup.com/mac-address-generator)生成随机且不重复的地址。

**注意：eFuse 数量有限，因此 MAC 地址只能修改有限次数。**

<a id="wi-fi--bluetooth-not-working-on-mainline-linux"></a>
### 主线 Linux 下 Wi-Fi / 蓝牙无法工作

最可能的原因是上游缺少所需固件。检查 `dmesg` 中是否有固件加载错误；通常可手动将所需二进制固件复制到 `/usr/lib/firmware`。

例如，Khadas Edge2 板载 AP6275P 模块（BCM/SYN43752）可使用：

```bash
sudo wget https://github.com/armbian/firmware/raw/refs/heads/master/brcm/brcmfmac43752-pcie.bin -P /usr/lib/firmware/brcm/
sudo wget https://github.com/armbian/firmware/raw/refs/heads/master/brcm/brcmfmac43752-pcie.clm_blob -P /usr/lib/firmware/brcm/
sudo wget https://github.com/armbian/firmware/raw/refs/heads/master/brcm/brcmfmac43752-pcie.txt -P /usr/lib/firmware/brcm/
sudo wget https://github.com/armbian/firmware/raw/refs/heads/master/brcm/BCM4362A2.hcd -P /usr/lib/firmware/brcm/
```

完成后重启。

<a id="advanced-troubleshooting"></a>
## 高级排查

`DEBUG` 构建会向串口输出详细日志。按[项目构建说明](../README.md#building)添加 `-r DEBUG` 构建。

1. 用调试镜像替换现有固件。
2. 参考厂商文档，将开发板的 **UART2** RX、TX、GND 连接至另一台电脑的 UART 转接器。
3. 打开串口终端（Windows 可用 PuTTY，Linux 可用 stty 配置），设置 1500000 波特率、8N1。
4. 给设备上电。

此时应能看到大量调试信息。若没有输出，请检查接线（交换 RX/TX）、转接器状态和配置。日志可帮助定位问题，需要协助分析时可提交 issue。

<a id="reporting-issues"></a>
# 反馈问题

UEFI 相关问题请提交到 [flange-build/edk2-flange](https://github.com/flange-build/edk2-flange/issues)。请尽量附上预期行为、实际结果、复现步骤及[串口日志](#advanced-troubleshooting)，并先检查是否已有相同问题。

<a id="building"></a>
# 构建

目前仅支持 Linux 构建；Windows 可使用 WSL。

1. 安装依赖。Ubuntu/Debian：

   ```bash
   sudo apt install git gcc g++ build-essential gcc-aarch64-linux-gnu acpica-tools python3-pyelftools uuid-dev python-is-python3 device-tree-compiler
   ```

   Arch Linux：

   ```bash
   sudo pacman -Syu
   sudo pacman -S git base-devel gcc dtc aarch64-linux-gnu-binutils aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc python python-pyelftools iasl --needed
   ```

2. 克隆仓库：

   ```bash
   git clone https://github.com/flange-build/edk2-flange.git --recursive
   cd edk2-flange
   ```

3. 构建 UEFI（以 ROCK 5B 为例，其他设备见[平台配置列表](../configs)）：

   ```bash
   ./build.sh --device rock-5b --release Release # 或 Debug
   ```

构建失败可能是依赖缺失。上表并非覆盖所有发行版的完整依赖清单，可根据具体错误信息补充软件包。

共用依赖、补丁集处理与输出文件见[项目构建说明](../README.md#building)。Qualcomm 使用独立脚本，详见 [Qualcomm 指南](../edk2-qualcomm/README.md)。

<a id="notes"></a>
# 补充说明

<a id="flash-layout"></a>
## 闪存布局

| 地址    | 大小       | 说明                  | 文件                   |
| ---------- | ---------- | --------------------- | ---------------------- |
| 0x00000000 | 0x00004400 | GPT 分区表             | rk3588_spi_nor_gpt.img |
| 0x00008000 |            | IDBlock               | idblock.bin            |
| 0x00100000 | 0x00500000 | BL33_AP_UEFI FV       | ${DEVICE}_EFI.itb      |
| 0x007C0000 | 0x00010000 | NV_VARIABLE_STORE     |                        |
| 0x007D0000 | 0x00010000 | NV_FTW_WORKING        |                        |
| 0x007E0000 | 0x00010000 | NV_FTW_SPARE          |                        |

固件镜像不包含变量存储，避免更新时覆盖用户设置。固件依赖这些固定偏移，不要修改。

<a id="memory-map"></a>
## 内存映射

| 地址    | 大小       | 说明                  | 文件                |
| ---------- | ---------  | --------------------- | ------------------- |
| 0x00040000 |            | ATF                   | bl31_0x00040000.bin |
| 0x000f0000 |            | ATF                   | bl31_0x000f0000.bin |
| 0x00200000 | 0x00500000 | UEFI FV               | BL33_AP_UEFI.Fv     |
| 0x007C0000 | 0x00010000 | NV_VARIABLE_STORE     |                     |
| 0x007D0000 | 0x00010000 | NV_FTW_WORKING        |                     |
| 0x007E0000 | 0x00010000 | NV_FTW_SPARE          |                     |
| 0x08400000 |            | OP-TEE                | bl32.bin            |
| 0xff100000 |            | ATF (PMU_MEM)         | bl31_0xff100000.bin |

<a id="licenses"></a>
## 许可

大部分 UEFI 代码使用 EDK2 默认的 [BSD-2-Clause-Patent](https://github.com/tianocore/edk2/blob/master/License.txt) 许可。

部分从 Linux 和 Rockchip U-Boot 移植的组件采用 **GPL-2.0**，请查看各文件的 `SPDX-License-Identifier`。

`misc/rkbin/` 中部分二进制文件的许可见 [rkbin LICENSE](https://github.com/rockchip-linux/rkbin/blob/master/LICENSE)。该目录也包含由 U-Boot（SPL）、Arm Trusted Firmware 和 OP-TEE 等开源项目构建的二进制，其许可可能不同。

<a id="community"></a>
## 社区

- [Hack w/ Rockchip Telegram](https://t.me/UEFIonRockchip)
- [Windows on R Discord](https://discord.gg/vjHwptUCa3)

<a id="credits--alternatives"></a>
## 致谢与其他项目

本固件基于 [Rockchip 最初的 UEFI 工作](https://gitlab.com/rk3588_linux/rk/uefi-monorepo)。

RK356x 用户可参考 [Quartz64-UEFI](https://github.com/jaredmcneill/quartz64_uefi)，本项目也复用了其中部分代码。
