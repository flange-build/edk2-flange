# FLANGE / LVGL 启动控制台

共享的 UEFI 图形启动菜单，已接入 RK3588 和 QCS6490 的 DSC/FDF。
主入口是 `FlangeUi.inf`，LVGL 固定为 **v9.3.0**，提交
`c033a98afddd65aaafeebea625382a94020fe4a7`。首次拉取后运行：

```sh
git submodule update --init edk2-common/ThirdParty/lvgl
```

所有标准 HII 层级共用新的 LVGL 显示：设备管理、启动维护、安全启动、
文件浏览、选择器、数值/字符串/密码输入以及保存和错误弹窗。首页用 `T` 打开
主题菜单，可选择 **Field、Terminal、Paper、Wartime、原版 HII**。
选择保存在 `FlangeMenuTheme` 启动服务非易失变量中，损坏或未知值回到 Field。
原版 HII 首页也有“菜单主题”入口，可以随时切回图形界面。

`FlangeForms.c` 接入现有 Form Display Engine 和 HII Popup 协议；设置存储、
表达式、验证、回调、层级导航和提交仍由 SetupBrowser 管理。显示适配只在
FlangeUi 存续期间生效，应用返回前恢复原协议，因此 LVGL 和字体只需存储一份。
`edk2-patches/0010-*` 给 UefiLib 的直接弹窗和 UiApp 的主题入口提供可选协议；
没有图形菜单服务时，原有文本行为保持可用。

## 设计约定

参考 arknights-printer 的四种设计方言，默认 Field，原创于 FLANGE 的固件启动场景，
不使用游戏标志、派系名或参考作品布局。

- 发布主体：FLANGE；用途：固件启动控制台；密度：标准。
- 底色 `#f2f2ef`，表面 `#fafaf8`，文字 `#191919`，状态强调 `#fff200`。
  黄色只标记当前选择和启动操作；不把设备类别映射为多种颜色。
- 单一母题：法兰接合处的两段相对括角。从页眉标记延伸到选中行、详情框、
  低对比度背景。连接所选行与详情的曲线由当前选择位置计算。
- 三层：模糊的括角背景、细点阵、清晰的文字与细线面板。无投影、表面发光
  或装饰性危险条纹。分页、设备路径与状态都对应真实数据。
- 中文：**Noto Sans SC 600**（正文 24px，标签 18px）；英文：**Archivo 600**
  （宽度轴 85，14/20/28px）；编号和路径：**IBM Plex Mono**（14/20px）。
  所有字体均嵌入固件，普通构建和运行不需要下载字体。
- 预览是同一份 `FlangeView.c` 的 LVGL 软件渲染结果；预览启动项是测试数据，
  实机上从 BootOrder/Boot#### 读取。菜单不显示虚构的设备性能或安全状态。

| 主题 | 配色与排版 |
| --- | --- |
| Field | 浅灰、信号黄、窄体标签 |
| Terminal | 深蓝黑、青色、等宽页眉和轻微扫描线 |
| Paper | 暖纸色、橄榄黄、小字号粗黑体 |
| Wartime | 浅灰、炭黑页眉、绯红切线与选区 |
| HII | 原有 UEFI 文本界面 |

## 操作和运行边界

| 操作 | 按键 |
| --- | --- |
| 选择启动项 | 上 / 下，Home / End；也可点击行 |
| 翻页 | PageUp / PageDown，或页码右侧的 `<` / `>` |
| 启动一次，不更改 BootOrder | Enter，或点击启动按钮 |
| 打开固件设置（所有层级沿用当前主题） | F2 |
| 打开五种主题的选择菜单 | T |
| 重新发现设备与启动项 | R |
| 切换英语 / 简体中文并保存 PlatformLang | L |
| 返回 BDS，继续其启动流程 | Esc |

- 保持当前 GOP 模式；支持 800×600 至 7680×4320。无 GOP、低分辨率或
  初始化/BLT 失败时调用原有 UiApp。串口仍可用 F2 进入原有菜单。
- 显示通过 GOP `Blt`，不依赖帧缓冲 RGB/BGR 排列或可直接寻址的 framebuffer。
  使用 64 行局部缓冲，LVGL 使用固定 4 MiB 内存池。
- 键盘使用固件聚合的 ConIn（包括串口）；鼠标使用 SimplePointer。
  **目前未接入 AbsolutePointer 触摸输入**。10ms UEFI 定时事件驱动等待，
  时间戳来自 TimerLib 性能计数器，单线程运行在 TPL_APPLICATION。
- 调用设置或启动程序前释放 LVGL 对象、显示缓冲、定时事件并恢复文本控制台。
  子程序返回后恢复进入菜单前的 GOP 模式，再重新获取显示信息和启动项。
  启动失败会显示 EFI 错误码。
- 隐藏和禁用的启动项，以及新旧菜单自己的 FV 文件项，不出现在列表。
- 中文正文包含 GB2312 字符集和界面额外字符；其他 Unicode 字符可能显示
  缺字方框。UTF-16 转换保留完整码点，并处理代理对、HII 宽度标记与截断。
- 设置入口按 GUID 查找已发布的固件卷，通过 LoadImage 的正常认证路径启动
  原有 UiApp。两套平台均继续打包旧 UiApp，图形菜单也可从独立 EFI 文件启动。

## 构建和验证

沿用项目的板级构建命令。对于已经应用补丁且存在本地修改的开发目录，使用
`--skip-patchsets`，避免构建脚本重置上游子模块：

```sh
./edk2-qualcomm/build.sh -d rubikpi3 -r RELEASE --skip-patchsets
./build.sh -d rock-5b -r RELEASE --skip-patchsets
```

主机端不需要 SDL 或显示服务器：

```sh
cmake -S edk2-common/Applications/FlangeUi -B workspace/flange-preview \
  -DFLANGE_SANITIZE=ON
cmake --build workspace/flange-preview -j 8
ctest --test-dir workspace/flange-preview --output-on-failure
workspace/flange-preview/flange-preview 1920 1080 zh workspace/flange-preview/preview.ppm
```

输出路径之后可传条目数（0–19，默认 4），再传主题编号（0–3），再传任意
参数可渲染五项主题选择页。输出为 PPM，可用 Pillow 转成 PNG。测试覆盖四种
主题的表单与输入框、只读项目、分页、密码遮挡、输入长度与代理对删除、
64 位有符号/无符号数溢出、日期/闰年、鼠标分页、UEFI Enter（CR/13）和
LVGL Enter（LF/10）启动、空列表与多次重绘，使用
AddressSanitizer / UndefinedBehaviorSanitizer。

本次本地验证：RubikPi3 RELEASE / DEBUG、ROCK 5B RELEASE 完整 FV 构建，
以及 5 组 ASan/UBSan 主机测试。QEMU 使用包含本项目补丁和菜单的完整 X64
OVMF 固件，验证多级启动维护、数值编辑、保存提示、安全启动确认、主题菜单、
HII 来回切换与变量持久化。QEMU 测试使用独立的模拟 NVRAM。

2026-10-10 已将 RubikPi3 Release 固件刷入 UEFI A/B 分区，两份镜像均经
读回逐字节校验。实机验证了 Esc 进入选择器、串口 Enter 触发启动，用户确认
显示器上已开始启动系统。GOP 性能、USB 输入和设备专属表单仍需进一步验证。

从旧 UiApp 升级时，已有的 Esc/F2 `Key####` 变量可能仍指向旧菜单，固件
不会覆盖已存在的快捷键。若升级后仍进入旧 HII，可在 UEFI Shell 使用
`bcfg boot dump -v` 和 `dmpstore Key*` 核对绑定，仅删除指向旧 UiApp 的
Esc/F2 变量，然后重启让固件重建。不要删除系统启动项或全部 NVRAM。

## 设置页操作

- 上下键、Home/End 选择；PageUp/PageDown 翻页；点击项目后按 Enter 或确认按钮。
- 说明区域可用鼠标滚动或左右键滚动；长项目名称和值在说明区域完整显示。
- 只读、锁定、灰显和被抑制的设置遵循 HII 属性。
- 启动顺序编辑中 `-` 上移、`+` 下移；Enter 应用，Esc 放弃本次排序。
- 文本/数值在输入框键入，Backspace 删除末尾；密码遮挡显示，验证旧密码、
  两次新密码一致性，并清除临时密码缓冲。
- F9/F10 等快捷键使用浏览器提供的操作与默认值类别，执行前显示确认。
- Esc 返回上一层；有未保存修改时提供保存、放弃、继续编辑三种选择。
- 单选、勾选、数字、字符串、密码、顺序列表、日期、时间、引用、操作和
  恢复默认值按钮均通过浏览器适配。自定义厂商图形 opcode/动画不作为菜单
  装饰绘制；标准表单文字和设置操作仍然可用。

## 字体重生成

生成后的字体 C 文件和许可证已纳入源代码。只有修改字体或字符覆盖时才需要：

```sh
python3 -m venv ark-work/flange-ui/font-python
ark-work/flange-ui/font-python/bin/pip install fonttools==4.60.1
npm install --prefix ark-work/flange-ui/font-tools --ignore-scripts lv_font_conv@1.5.3
ark-work/flange-ui/font-python/bin/python \
  edk2-common/Applications/FlangeUi/Tools/generate_font.py \
  --cache ark-work/flange-ui/fonts \
  --converter ark-work/flange-ui/font-tools/node_modules/.bin/lv_font_conv
```

字体来源及 SHA-256 见 `Fonts/sources.json`，锁定 Google Fonts 提交；生成器会
校验下载内容，实例化字重/宽度轴，生成 4bpp 抗锯齿压缩字体。
字体采用 OFL-1.1，LVGL 采用 MIT，本项目集成代码采用 BSD-2-Clause-Patent。
上游参考：[LVGL 字体转换器](https://github.com/lvgl/lv_font_conv)、
[LVGL UEFI 接口文档](https://lvgl.io/docs/open/9.3/details/integration/driver/uefi.html)。
这里使用项目自己的局部 GOP 缓冲和 ConIn 适配，以保持原有显示模式与串口输入。
