# Windows 11 on Arm 调研结论（RUBIK Pi 3 / QCS6490）

日期：2026-10-02。状态：**只做了调研，未实施，已决定暂不做**。详细计划见 `PLAN.md`；各专题报告见同目录的 `*.md`，索引见 `README.md`。

## 起因

Rufus 制作的 Win11 25H2 ARM64 安装盘从 USB 3.0 口启动，bootmgr 报 `BlInitializeLibrary failed 0xc0000225`。

## 根因（已从 bootaa64.efi 反汇编确认）

- `BlInitializeLibrary`（`0x101aedb8`）在 `0x101af6cc` 按签名取 **FACP（FADT）**。查找过程是：在 EFI 配置表里找 ACPI 2.0/1.0 的 RSDP，再经 XSDT 找到 FADT。找不到就返回 `STATUS_NOT_FOUND`（0xC0000225），且这一步是致命的。
- 同一函数在 `0x101afab4` 取 MCFG，这一项是可选的。
- 我们的固件只提供设备树，没有任何 ACPI 表，所以失败。
- 日志里的 `ConvertPages: failed to find range 102000 - 102FFF` 是 **误导**：bootmgr 入口（`0x1003a24c`）在固定地址 0x102000 预留一页，但根本不检查返回值。
- winload 另外还要求 APIC（MADT）；内核需要 GTDT，以及 PSCI 1.x。

## 要支持 Windows 需要什么

**最小 ACPI 表集**：RSDP rev2 + XSDT、FADT rev6（HW_REDUCED，PSCI 走 SMC）、MADT（GICv3：GICD 0x17A00000，GICR 0x17A60000）、GTDT（PPI 29/30/27/26）、DSDT。调试可再加 DBG2/SPCR，GENI 串口子类型为 0x11 或 0x13。

**Windows 自带驱动能覆盖的部分**（已从 ISO 的 boot.wim 里的 inf 确认）：

| 设备 | 驱动 | ACPI 描述 |
|---|---|---|
| UFS | `storufs` | `ACPI\QCOM24A5` |
| usb_2 | `usbxhci` | `PNP0D10`/`PNP0D15` |
| Renesas USB 3.0 | `usbxhci` | 走 PCI，但需要 PCIe 能以 ECAM 方式访问 |
| AX88179 以太网 | 自带驱动 | — |
| 显示 | BasicDisplay | 使用 GOP 帧缓冲，前提是 ExitBootServices 后 HDMI 不关 |

**最有力的参照**：Radxa Dragon Q6A（同为 QCS6490）官方支持 Windows 11。不装驱动就能用 HDMI（GOP）、UFS、USB 2/3、NVMe；装上他们的驱动包后，GPU、视频编解码、GPIO 等也能用。Wi-Fi 和 BT 不行。这个驱动包里 `qcpep6490.sys` 是关键，缺了它会蓝屏 0x14E。

## 估计的工作量和阶段

| 阶段 | 内容 | 工作量 |
|---|---|---|
| 0 | 硬件小实验，准备安装盘 | 3～6 天 |
| 1 | ACPI 最小集；设备树/ACPI 切换；Windows 启动时 HDMI 不关、SMMU 不还原；首选 EL2 | 10～18 天，目标是看到 WinPE 安装界面 |
| 2 | DSDT 加 UFS 和 usb_2；运行时变量先写内存；BDS 自动加 Windows 启动项 | 8～15 天，目标是能装到 UFS |
| 3 | 把 PCIe0 包装成 ECAM，用上 USB 3.0 和有线网 | 6～15 天 |
| 4 | 日常可用（RTC、RNG、变量持久化、安全启动等） | 30～60 天 |
| 6 | 高通驱动栈（PEP、GPU、DSP） | 不定，取决于驱动来源和许可，且需要 EL1（只有在 Gunyah 下 TrustZone 才会运行 DSP） |

只用 Windows 自带驱动、做到日常可用（A 层），大约需要 2.5～4 个月。

## 硬阻碍

- 板载 Wi-Fi（AP6256，SDIO）没有 ARM64 驱动。
- HDMI 经 LT9611 桥接，高通显示驱动不支持，只有基本显示。
- 没有 PEP，就没有调频、温控和睡眠。
- 没有 TPM，Windows 11 需要绕过检查。
- PCIe 不是 ECAM。
- Windows 会看到全部 UFS LUN，包括放固件的那几个。

## 如果以后重启这项工作

- 从 `PLAN.md` 的阶段 0 开始，先定第 7 节的决策：版本、异常级别、磁盘布局、驱动来源。
- 高通派生的 ACPI 表和驱动一律不进 git。
- 报告里引用的 `bin/`、`dis/`、`q6a-fw/`、`fwgap/` 等中间文件（解出的二进制、反汇编、解包结果）不在仓库里，可以从 ISO 和 Radxa 刷机包重新生成。
