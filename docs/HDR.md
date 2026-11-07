# 原生 HDR 输出

`[HDR] HDR = On` 让游戏在 HDR 显示器上以 **HDR10（BT.2020/PQ）** 或 **scRGB（线性 BT.709，FP16）** 输出；
`HDRFormat = Auto` 优先 HDR10。默认关闭；关闭时完全不启用相关路径。

实现参考 RenoDX 的 MHGU HDR mod（souperman9 / ShortFuse，MIT），但直接做在渲染器里，不使用 ReShade。

**状态：已编译，尚未在真实 HDR 显示器上完整验收；使用前建议先关闭 FSR。**

## 原理（简述）

- 颜色分级后的后处理链（B/H/P）改用 RGBA16F，避免 SDR 白点处提前截断。
- `UnclampedScene = On` 时，场景、景深、Bloom 链等其余 RGBA8 目标也使用 16F，让阳光、火、火花等
  高于白色的亮度进入颜色分级。
- 游戏自己的颜色分级与 Bloom 加法使用 HDR 变体：先解码到线性光，再做曝光/对比度/高光/阴影/饱和度、
  RenoDRT 色调映射与 gamma 校正。
- 最终合成图按 `UIBrightness` 缩放、按 `PeakBrightness` 在 BT.2020 中裁剪，再写 PQ 或 scRGB，并附带
  `VK_EXT_hdr_metadata` 元数据。
- FXAA、HUD 与游戏亮度 pass 保持原样；HUD 在 [0,1] 内，按 UI 亮度显示。

## 设置

| 键 | 范围 | 默认 | 说明 |
|---|---|---|---|
| `HDR` | Off/On | Off | 打开原生 HDR |
| `PeakBrightness` | 80–10000 nits | 1000 | 显示器峰值亮度 |
| `GameBrightness` | 48–500 nits | 203 | 游戏白点 |
| `UIBrightness` | 48–500 nits | 203 | HUD/菜单/overlay |
| `ToneMapper` | Vanilla/None/RenoDRT | RenoDRT | 仅 INI |
| `GammaCorrection` | Off/2.2/BT.1886 | 2.2 | |
| `Exposure` | 0–2 | 0.65 | 1 = 游戏原样 |
| `Highlights` / `Shadows` | 0–100 | 60 / 50 | 50 = 游戏原样 |
| `Contrast` / `Saturation` / `HighlightSaturation` | 0–100 | 50 / 50 / 82 | |
| `Flare` | 0–100 | 0 | 仅 INI |
| `Bloom` | 0–100% | 100 | 仅 INI |
| `UnclampedScene` | Off/On | On | 场景也使用 16F |
| `HDRFormat` | Auto/HDR10/scRGB | Auto | 仅 INI |
| `Analysis` | Off/On | Off | HDR 分析 overlay |

`MHGU_HDR=0|1` 可覆盖 `HDR`。

FSR 与 HDR 互斥：HDR 打开时 FSR 显示为 “Off (HDR)”，画质与性能设置里的 FSR 会在下次启动完全关闭。
如果正在使用 FSR 时打开 HDR，该行会显示 `*`，需要重启。

## HDR 分析

设置里的 **HDR analysis** 会显示：
- 左上：画面最大/平均/最小亮度（nits）、屏幕中心亮度、BT.709 / DCI-P3 / BT.2020 覆盖率；
- 右下：按屏幕列统计的亮度波形（对数刻度，红色 = PeakBrightness，黄色 = GameBrightness）。

这些数值来自 GPU 对最终画面的采样，Android 也可用（无需 ReShade）。

## 限制

- FSR 在 HDR 下关闭（FSR 的输入仍是 RGBA8）。
- `UnclampedScene` 会让高于 1 的值进入原本看不到这些值的 shader；NaN/Inf 已在分级中清理，但个别特效
  可能表现不同，异常时把它关掉（后处理链仍是 16F）。
- 截图（`MHGU_SHOT_AT` / 控制文件的 `shot`）是合成图，即中间编码，并截到 8 bit。
- 需要显示器与驱动支持 HDR；Windows 需先在系统显示设置里打开 HDR。

参考实现：RenoDX（MIT，https://github.com/clshortfuse/renodx），以及其 MHGU mod。
