# 实机验证 / Device validation

2026-10-10，Thundercomm RubikPi3，Ubuntu 26.04.1 ARM64，
`7.0.0-38-generic`，UEFI EL2，Linux VHE，Mesa 26.0.8。

DKMS 3.2.2 已从 deb 包所含源码成功编译、签名并安装 `rubikpi3-msm/0.1.0`，
模块路径为 `updates/dkms/msm.ko.zst`。编译日志包含编译器命令名及 pahole
版本差异提示，未发生构建错误。

DKMS 3.2.2 successfully built, signed and installed `rubikpi3-msm/0.1.0`
from the sources included in the deb. The selected module is
`updates/dkms/msm.ko.zst`. Compiler command-name and pahole version warnings
were present; the build completed without errors.

卸载测试恢复了 Ubuntu 原版模块，并通过原始 SHA256 校验：
`8141920291ed0b49137e1b6071385eb3a3496fa8eaf511a66e31b52235e424f2`。
包自带的 modprobe 配置已随卸载移除，initramfs 更新成功；随后重新安装并重启。
没有通过在线卸载 MSM 切换驱动，也没有重启进入已回退的原版模块做渲染测试。

Removal restored the stock Ubuntu module with the original SHA256 above,
removed the packaged modprobe configuration and refreshed initramfs. The DKMS
package was then reinstalled and the device rebooted. MSM was never unloaded
live. Rendering with the restored stock driver was not retested after reboot.

重启后 / After reboot:

- 磁盘与运行中的模块 srcversion 一致 / On-disk and loaded module srcversion match:
  `8CA61C45CED275A8443D1EE`.
- `dkms status`: `rubikpi3-msm/0.1.0, 7.0.0-38-generic, aarch64: installed`.
- `zap_direct_el2=Y`, `separate_gpu_kms=Y`；日志确认 EL2 直接 SECVID 路径生效。
  Kernel logs confirm the direct SECVID path at EL2.
- 普通用户运行 EGL：FD643，128 次 GPU 清屏和像素回读全部通过，含 2 秒空闲恢复。
  EGL as an unprivileged user: FD643, 128 GPU clears/readbacks passed, including
  a two-second idle interval.
- OpenGL 4.6 / GLES 3.2，Vulkan 枚举为 / Vulkan enumerates
  `Turnip Adreno (TM) 643`, API 1.3.335。未执行 Vulkan 绘制测试 / Vulkan drawing was not tested.
- GDM 服务正常运行 / GDM is active.
- 测试期间未见 GPU fault、hangcheck 或 CP_SET_SECURE_MODE 错误。
  No GPU fault, hangcheck or CP_SET_SECURE_MODE errors were observed during testing.

仅上述内核已实测；其他 ABI、跨内核大版本、Secure Boot 强制验证、EL1/nVHE
及休眠恢复均未验证。
Only the kernel above has been tested. Other ABIs, kernel series, enforced
Secure Boot, EL1/nVHE and suspend/resume have not been validated.
