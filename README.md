# edk2-flange

**简体中文** | [English](README.en.md)

基于 [TianoCore EDK II](https://github.com/tianocore/edk2) 的 ARM64 UEFI 固件，支持 Rockchip RK3588 和 Qualcomm QCS6490 开发板，提供固件设置、启动管理和板级硬件初始化，并共享启动标志与中英文菜单。

图形菜单基于 LVGL，首页、各级设置和弹窗统一支持 Field、Terminal、Paper、Wartime 四种风格，也可切回原版 HII。首页按 `T` 打开主题菜单，选择会保存到下次启动；原版 HII 首页同样提供主题入口。字体、操作与构建说明见 [菜单界面文档](edk2-common/Applications/FlangeUi/README.md)。

Rockchip 支持源自 [edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588)。Qualcomm 支持基于 [edk2-platforms](https://github.com/tianocore/edk2-platforms) 的 RB3 Gen 2 移植，加入本地 QCS6490 驱动及 Thundercomm RUBIK Pi 3 平台。

<a id="platforms-and-guides"></a>
## 平台与指南

| 平台 | 构建入口 | 输出文件 | 使用指南 |
| --- | --- | --- | --- |
| Rockchip RK3588 / RK3588S 开发板 | `./build.sh -d <device>` | `RK3588_NOR_FLASH.img` | [Rockchip 配置、开发板列表与系统兼容性](edk2-rockchip/README.md) |
| Thundercomm RUBIK Pi 3（QCS6490） | `./edk2-qualcomm/build.sh -d rubikpi3` | `RUBIKPI3_UEFI.elf` | [Qualcomm 配置、刷写与硬件状态](edk2-qualcomm/README.md) |

Rockchip 设备名称见 [configs](configs/)，Qualcomm 设备名称见 [edk2-qualcomm/configs](edk2-qualcomm/configs/)。各平台的固件镜像和刷写流程不可混用。

<a id="current-rubikpi3-validation"></a>
## RubikPi3 当前验证结果

截至 2026-10-10，官方 **Ubuntu Desktop 26.04.1 ARM64** 安装镜像（内核 `7.0.0-30-generic`）已通过 USB 启动进入 Live 桌面与安装器，使用的是**未经修改的原始 GRUB 启动项**，同时可识别 UFS 存储。配合仓库自带的板级设备树，无需添加 `clk_ignore_unused`、`pd_ignore_unused` 或驱动黑名单参数。

在 **设备管理器 → 平台配置（Device Manager → Platform Configuration）** 中设置以下选项，保存并重启：

| 设置 | 已验证的值 |
| --- | --- |
| Hypervisor（虚拟机监控程序） | **EL2 (KVM)** |
| DSP Preload（预加载 DSP） | **Disabled** |

HDMI 保留 UEFI 初始化的 1920×1080@60 DPU/DSI/LT9611 配置，并将帧缓冲交给 Linux `simpledrm`。设备树保持显示供电，使 Linux 在清理未使用资源之前接管显示时钟和电源域。本次验证覆盖固件帧缓冲输出；尚未验证原生显示模式设置、GPU 加速及休眠恢复。

板级设备树禁用了会触发安装器内核故障的 ICE 和 GPI 节点，同时保留 UFS 访问。DSP 预加载仍可用于其他配置，但近期测试中，Ubuntu 安装器在 DSP Auto 配置下未能进入 Linux。请对上述已验证配置保持 Disabled；具体内部故障仍在排查。

此结果仅覆盖 **Live 环境启动**，尚未完成完整系统安装验证。安装器曾损坏开发板的 LUN4 启动分区。安装前请核对目标磁盘和分区变更，不要擦除固件 LUN，也不要将所有暴露的 UFS 磁盘都当作系统存储。当前步骤与限制见 [Qualcomm 指南](edk2-qualcomm/README.md#ubuntu-desktop-installer)。

<a id="building"></a>
## 构建

请在 Linux 或 WSL 等 Linux 环境中构建。两个平台的构建脚本均使用仓库固定版本的子模块和本地补丁集。

<a id="get-the-sources"></a>
### 获取源码

```sh
git clone --recursive https://github.com/flange-build/edk2-flange.git
cd edk2-flange
```

已有工作区可运行 `git submodule update --init --recursive` 初始化缺失的子模块；操作前请保存子模块中的本地修改。

<a id="dependencies"></a>
### 安装依赖

Debian / Ubuntu 可安装以下软件包，覆盖两个平台使用的工具及 Qualcomm ELF 打包依赖：

```sh
sudo apt install acpica-tools binutils-aarch64-linux-gnu build-essential \
  device-tree-compiler gcc-aarch64-linux-gnu gettext git \
  libc6-dev-arm64-cross python3 python3-cryptography python3-pyelftools uuid-dev
```

<a id="build-a-board"></a>
### 构建目标开发板

在仓库根目录选择对应命令执行：

```sh
# Qualcomm：Thundercomm RUBIK Pi 3
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE

# Rockchip：Radxa ROCK 5B
./build.sh -d rock-5b -r RELEASE
```

省略 `-r` 时，两个脚本均默认构建 `DEBUG`。输出文件复制到仓库根目录，中间文件位于 `workspace/`。Rockchip 默认使用开源 TF-A 子模块。各平台其他选项可通过脚本的 `--help` 查看。

**修改子模块时请注意：** 常规补丁应用步骤可能在目标子模块内运行 `git reset --hard` 和 `git clean -xfd`。首次完成必要的补丁应用后，开发构建可使用 `--skip-patchsets` 保留本地修改：

```sh
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE --skip-patchsets
./build.sh -d rock-5b -r RELEASE --skip-patchsets
```

全新克隆、尚未应用必要补丁的工作区不能直接跳过补丁步骤。

仓库的 [GitHub Actions 构建矩阵](.github/workflows/build.yml) 目前覆盖 Rockchip 开发板。Qualcomm 的构建和硬件验证单独进行；当前发布工作流不生成 RubikPi3 固件。

<a id="flashing-and-first-boot"></a>
## 刷写与首次启动

请遵循对应平台的指南：

- **Rockchip：** 将匹配开发板的镜像写入 SPI NOR、SD 或 eMMC，见[安装与更新](edk2-rockchip/README.md#getting-started)。
- **RubikPi3：** 保留与开发板匹配的 Qualcomm 启动固件，通过 EDL 将 UEFI ELF 写入已确认的 `uefi_a` / `uefi_b` 分区。刷写前备份分区，写入后读回校验，见 [Qualcomm 刷写说明](edk2-qualcomm/README.md#flashing)。

串口参数也随平台不同：RubikPi3 调试 UART 为 **115200 8N1**，RK3588 UART2 为 **1500000 8N1**。使用简体中文菜单时，终端应支持 UTF-8，并安装含 CJK 字形的字体。

<a id="repository-layout"></a>
## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `edk2/`、`edk2-platforms/`、`edk2-non-osi/` | 上游 EDK II 子模块 |
| `edk2-common/` | 共用启动标志 |
| `edk2-rockchip/`、`configs/`、`build.sh` | Rockchip 平台、配置与构建入口 |
| `edk2-qualcomm/` | Qualcomm 平台、芯片驱动、构建脚本与文档 |
| `edk2-patches/`、`edk2-qualcomm/edk2-platforms-patches/` | 应用于上游 EDK II 源码的本地补丁 |
| `devicetree/` | 主线与厂商设备树、主线补丁集 |
| `arm-trusted-firmware/`、`arm-trusted-firmware-patches/` | Rockchip 使用的 TF-A 源码与补丁 |
| `misc/`、`edk2-rockchip-non-osi/` | Rockchip 打包工具与二进制组件 |
| `workspace/` | 构建生成文件，由 Git 忽略 |

RubikPi3 板级设备树为 [`qcs6490-thundercomm-rubikpi3-mainline.dts`](edk2-qualcomm/Platform/Thundercomm/RubikPi3/DeviceTree/qcs6490-thundercomm-rubikpi3-mainline.dts)，在基础板级设备树上加入固件交接与安装器兼容性覆盖。帧缓冲地址和显示模式由固件在运行时通过 GOP 填入。

<a id="reporting-issues"></a>
## 反馈问题

请在 [flange-build/edk2-flange](https://github.com/flange-build/edk2-flange/issues) 提交问题，附上开发板、提交版本、构建模式、启动固件包、系统与内核版本、UEFI 设置、复现步骤及串口日志。尽可能区分故障发生在 UEFI、系统引导器、内核还是桌面阶段。

Qualcomm Windows / ACPI 研究记录见 [Windows 研究文档](edk2-qualcomm/docs/windows/README.md)，目前尚不构成受支持的 RubikPi3 启动方式。Rockchip 系统兼容性见其[平台指南](edk2-rockchip/README.md#supported-oses)。

<a id="licenses-and-credits"></a>
## 许可与致谢

大部分固件代码采用 **BSD-2-Clause-Patent** 许可，个别组件使用其他许可。请检查各文件的 SPDX 标识及对应子模块的许可文件。部分移植代码采用 **GPL-2.0**；二进制固件另有各自的再分发条款。

感谢 TianoCore、edk2-rk3588 贡献者、上游 Qualcomm RB3 Gen 2 移植、Linux 设备树贡献者、qtestsign 及各开发板社区。Rockchip 的历史与社区链接保留在 [Rockchip 指南](edk2-rockchip/README.md#credits--alternatives)。
