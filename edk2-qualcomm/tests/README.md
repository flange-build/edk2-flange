# 主机回归测试 / Host regression tests

显示 DT 交接测试直接提取 `DeviceTree.c` 中的固件函数，用最小 UEFI mock 和
合成 DT 调用仓库内的真实 libfdt；不需要设备、DTB 样本或交叉编译工具链。

依赖：Python 3、支持 C11 的主机编译器（默认 `cc`，可设置 `CC`），以及已经初始化的
`edk2/MdePkg/Library/BaseFdtLib/libfdt` 子模块。测试直接编译该 libfdt，无需安装系统
`libfdt-dev`。仓库根目录运行：

```sh
git submodule update --init --recursive edk2
python3 edk2-qualcomm/tests/test_display_fixup.py
python3 edk2-qualcomm/tests/test_display_fixup.py --sanitize
```

`--sanitize` 需要编译器支持 AddressSanitizer 和 UBSan。生成的函数文件和二进制只放在
临时目录，测试结束自动删除。

覆盖 Ubuntu GRUB 的 `size=0` 查询、仅 `realloc` 后一次重试；容量不足时原树不变；
当前 GOP 地址/尺寸与资源属性；能力标记；无 framebuffer、无 GOP；固件初始树预留；
同一缓冲区重复调用保持成功且字节不变。真实 HDMI 输出仍需实机验证。

The test extracts the production firmware functions and runs them with small UEFI
mocks, synthetic device trees and the vendored libfdt. It requires Python 3, a C11
host compiler (`cc`, or `CC`) and initialized edk2/libfdt submodules. No board,
external DTB, system libfdt development package or cross compiler is required.

Run the commands above from the repository root. `--sanitize` additionally needs
AddressSanitizer and UBSan support. Generated sources and executables live only in
a temporary directory, which is removed afterwards.

Coverage includes Ubuntu GRUB's size-zero query and single realloc/retry,
unchanged input on insufficient capacity, current GOP data and resource retention,
the capability marker, missing framebuffer/GOP, initial firmware buffer sizing and
idempotent calls with the same allocation. HDMI output requires a hardware test.
