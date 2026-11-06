# 交叉对照审计（2026-11-04）

对照对象：`MHGURecomp/runtime`（C 版运行时，NVN/HLE/服务/内核）+ `gu-native`（游戏库 ABI 头与 `gamelib_entry.c`）。
方法：逐文件、逐入口把 C 实现与 `mhgu-linux` 的 Rust 实现并排读，标注「对齐 / 等价但写法不同 / 行为分歧 / 缺失」，
再按「是否导致画面错误或启动卡住」排序。行号均指各自仓库。

## 结论摘要

- IPC 编解码、NSO 载入/重定位、NVN 对象模型、命令记录、纹理 BC1–BC5 与 block-linear、内核 svc 的主干
  与参考逐条一致；本轮修掉的都是「同一处语义抄错」，不是架构差异。
- 当前黑屏有两条独立原因：
  1. 启动阶段游戏只清屏 + 全屏后处理，健康提示/开场 logo 属于 applet 与 `movie::`，两者都未实现
     （`_ZN5movie*` 全部是成功 stub，150 秒内一次未调用）。
  2. 画面链上的实现 bug（本轮已修，见下）会叠加：几何 draw 采样的纹理被解成全 0、常量缓冲编号错位、
     片元写掩码恒 0 等。
- 几何 draw 在「常量纯色片元」下已能覆盖 21% 的屏幕（`MHGU_GPU_NO_SMALL_DRAWS=1 MHGU_GPU_SOLID_FRAG=1`
  的目标回读），说明顶点/光栅/索引/属性/渲染目标路径已通；把片元换成「输出第一个 varying」后，
  该区域变成调试回退色（黄），说明覆盖该区域的 draw 本身不写 varying（纯色/常量着色器），
  而不是插值丢失：同一 pass 里 358 顶点的 draw 的 `vin=0x7 vout=0x3` 是正常的。剩下的是这类
  纯色 draw 读的常量/存储缓冲（cbuf0 的 SSD 表、storage buffer）与启动内容缺失。
- 后续（同日）修正这条判断：`MHGU_TRACE_FS` 按 RomFS 偏移反查文件名后，游戏实际在读
  `nativeNX/eng/arc/warning_eng.arc` 等**健康提示资源** —— 提示是游戏自己画的，不是系统 applet。
  `MHGU_GPU_DEBUG_TEX`（第一个 varying 当 UV、强制不透明）在几何 pass 目标上采出真实像素
  （0x6b5942 / 白色），说明纹理与插值都对；黑屏来自片元着色器本身。对照 `write_tex_result` 找到真 bug：
  TEXS 的 `msel` 掩码决定分量与目标寄存器（d0/d0+1/d1/d1+1，表 `[1,2,4,8,3,9,10,12]` /
  `[7,11,13,14,15,15,15,15]`），旧实现无条件写 `texel.x→d0, texel.y→d1`，r1/r3 残留坐标值、alpha 错。
  已按参考实现；`TEXS` 的 trace（`MHGU_TRACE_TEXS`）可见游戏用的是 `d822008020170000` 这类
  `info=1, msel=0, d1=2` 形式。

## 黑屏的最终根因（2026-11-05）

`EXIT` 被线性翻译成 `return;`，而阶段 epilogue（片元颜色输出 `c0..c7`、顶点的 `gl_Position` 修正）
在 `main` 里位于 body **之后**：于是每条着色器都在写颜色之前返回，所有 draw 实际什么都没写，
目标保持清屏黑。参考实现的线性路径把 `EXIT` 译成 `do { … } while(false)` 里的 `break`，epilogue 仍会执行。
修复（`translate_ex`/`translate_program`：EXIT 只结束指令流、不发射 `return`）之后，游戏立刻画出
真实画面：**1920×1080 的 “There is no data to load.”**（256 种颜色，文字由 `sysfont` 图集绘制）。

这一条也解释了为什么此前：纯色片元有画面、采样调试有画面、真实片元全黑 —— 因为调试着色器没有 EXIT。

## 本轮修复（按参考逐条对齐）

| 位置 | 参考 | 修复 |
|---|---|---|
| `nvn_cmdbuf.rs` SetViewport | `cmdbuf.c:197` 无签名（整数参数） | 原来是 `"ffff"`，坐标恒 0 且记录根本没写入 |
| `render.rs` 片元写掩码 | `exec.c:3800` `out_mask`（=片段颜色输出图） | 原来取变体掩码，几何 draw 的 `colorWriteMask` 恒 0 |
| `render.rs` 常量缓冲编号 | `exec.c:3135-3200`：cbuf 0 驱动表、1 编译常量、≥3 游戏 UBO | 原来游戏 UBO i 绑到 cbuf i，shader 读 cb3 拿到零 |
| `tex.rs` 存储地址 | `objects.c:331` `addr = pool + offset` | `storage()` 又加了一次 offset，纹理解出全 0 |
| `tex.rs` 线性判定 | `textures.c:553` `flags & 0x10` | 原来按 `stride != 0` |
| `tex.rs` 格式表 | `formats.c` | 0x51–0x6C 的 bpb 全写成 1/4；BGRA/RGBX/RGBA-sRGB 的 alpha 规则反了；补齐 16/32 位通道转换 |
| `render.rs` 顶点属性 | `exec.c:3785` ZERO_BINDING | 未绑定属性原来被跳过（Vulkan 未定义），改为零流 (0,0,0,1) |
| `render.rs`/`exec.rs` base vertex | `exec.c:4058` | 索引 draw 的 `vertexOffset` 原来硬编码 0 |
| `render.rs` 深度范围 / 图元重启 | `exec.c:1849`、`3823` | 补 `SetDepthRanges`、仅 strip 开启重启 |
| `exec.rs` 混合常量 | `exec.c:4409` | 补 `SetBlendColor` → `blend_constants` |
| `width`/呈现 | `device.c` 交换链 | 窗口按游戏尺寸（1920×1080）缩放呈现；`nvnQueuePresentTexture` 不再用清屏覆盖已渲染帧 |
| `nvn_hle.rs` texel 读写 | `objects.c:407-456` | 补 `WriteTexels`（线性→block-linear swizzle）与 `ReadTexels(Strided)` |
| `nvn_hle.rs` applet 事件 | `am.c:64`、`applets` | faked applet 的「已完成」事件改为 sticky（guest `ClearEvent` 后仍 signaled）；accessor 只对 cmd0 返回事件、cmd1/21 返回布尔/退出码 |
| `protocol` MAX_FRAME | `ldn.c:109` `0x10000+64` | 原 4096 会让 ≥4 个 hub 的 `LIST` 应答被拒 |
| `window.rs` | — | 呈现改 `vkCmdBlitImage` 缩放；已渲染帧不被 clear 覆盖 |

## 仍未对齐 / 未实现（按影响排序）

1. ~~`movie::` 缺失~~ → 已用 FFmpeg 实现（`runtime/movie_hle.rs`，布局/虚表同 `runtime/hle/movie.c`）：
   `MediaData`、`Buffer`、`Extractor`（解复用）、`Decoder`（解码到 NV12 / s16 音频，事件与缓冲池）。
   `tests/movie.rs` 用 ffmpeg 生成的 H.264 片段做端到端验证。启动时游戏仍未构造这些对象
   （`MHGU_TRACE_MOVIE`），开场 movie 之前还有一道门。
2. applet 内容与 `vi` 合成：健康与安全提示、系统层叠加都没有渲染。
3. 渲染器缺项（`exec.c` 有、Rust 无）：storage buffer/image 描述符与 cbuf0 驱动表；MRT（颜色 1..7）；
   stencil；MSAA/alpha-to-coverage；alpha test（alpha_ref/alpha_func）；逐附件写掩码；纹理采样器池
   （过滤/寻址/compare，目前全局 LINEAR/CLAMP）；实例化 divisor；`CMD_COPY_*`；
   `CMD_UPDATE_UNIFORM_BUFFER`（CPU 端写 guest）；多 viewport/scissor 槽位；深度范围 clamp。
4. 纹理：ASTC 解码器、BC6H/BC7、mip 链、3D/array/cube 目标、packaged block height、sRGB 视图。
   参考在没有 GPU 支持时软件解 ASTC/BC1-BC5；我们只做了 BC1-BC5。
5. 着色器翻译（`maxwell/translate.c` 对照）：本轮补了 `SEL/PSETP/PSET/IADD/IADD32I/ISCADD/ISCADD32I/
   SHF/IMUL32I/LOP/LOP32I/LOP3/FSET/FCMP` 以及谓词初始化与 `.CC` 进位链（`cc/ccz/ccs`）；
   仍缺 `XMAD`（四种编码）、半精度族（`HADD2/HMUL2/HFMA2/…`）、`ISET/ICMP/CSETP`、`F2F` 的 F16 形式、
   `I2I`、`PRMT` 的 cbuf 形式；`IPA` 模式未生效；`LDC` 动态双字重读、动态 cbuf 越界未 clamp；
   `gl_FragDepth`、alpha test、整数渲染目标、`gl_PointSize` 未生成；几何阶段未接。
   分支类也已按参考实现：`translate_ex` 先扫描 `BRA/SSY/PBK/PCNT/SYNC/BRK/CONT/BRX`，命中就生成
   `switch (pc)` 状态机（分支目标打 `case`、`norm_target` 跳过调度字、`EXIT` 置 `exit_now`），
   与 `translate.c:1919-1928/2000-2048` 一一对应；`0x50b0`（NOP/DEPBAR/MEMBAR）按 no-op 处理；
   顶点/片元 epilogue 用到的 `r0..r3` 现在总会声明（此前合成程序会因 `r3` 未声明编译失败）。
   `// unsupported` 只剩 `XMAD`（以及参考也未实现的少数类）。本轮结束时游戏用到的 20 个 shader：
   **0 条 unsupported、0 个编译/管线错误**。另外补了 `GMem`（binding 17）的零缓冲绑定，
   使 LDG/STG 的着色器至少能正确链接并确定性地读到 0（参考把 guest 堆映射上去）。
   片元输入按参考补齐：顶点阶段没写的 varying 不再声明为输入（Vulkan 要求输入必须是顶点输出），
   而是替换成常量（alpha=1，其余 0）；IPA 的 pass 模式把 varying 乘回插值 w（`1/gl_FragCoord.w`）。
   仍未做：storage buffer 描述符与 cbuf0 的 SSD 表、采样器池、alpha test、`gl_FragDepth`、
   整数渲染目标、几何阶段。
   （注意：参考的分支模式 `switch(pc)` 实际没有 `case` 标签，produces 空循环；两边都靠线性翻译，
   因此“分支未接”不是本项目的偏差，除非参考后来修过。）
6. 内核/ABI 审计：svc 主干一致，分歧集中在少数返回约定与诊断（另见 agent 报告 `history://AuditKernel`）。
7. 服务：`fsp-srv` 存档日志缺失、`time` cmd 20 共享内存缺失、`audout` 不返回会话对象、
   `service_lookup` 对任意名字都返回会话（参考只接受 `sm:`）、`set` 的 `MHGU_LANG`/`MHGU_REGION` 未生效。
8. 网络：Rust 客户端不发送 `MHGR` 中继前缀、无 station hello；`Welcome`/`NetworkInfo` 未做参考的校验；
   hub 的 ATTACH 后连接名额提前释放；Join/ATTACH 竞态等待 2s vs 参考无限等待。
9. 音频：只有周期性事件与静音输出，没有参考的 voice/ADPCM/子混音/效果链。

## 复现命令

```sh
# 启动到黑屏阶段，观察 pass/draw 与纹理地址
MHGU_TRACE_PASS=1 MHGU_TRACE_RT=1 ./target/release/mhgu-runtime --game-lib <libmhgugame.so> \
  --game-id <id> --root /path/to/MHGU-1.4 --run

# 几何 draw 的判定与目标内容（跳过全屏拷贝、片元换纯色）
MHGU_PAD_A=1 MHGU_GPU_NO_SMALL_DRAWS=1 MHGU_GPU_SOLID_FRAG=1 \
MHGU_GPU_DUMP=/tmp/f.ppm MHGU_GPU_DUMP_TARGET=all MHGU_GPU_DUMP_HEAVY=1 \
./target/release/mhgu-runtime --game-lib <lib> --game-id <id> --root <root> --run
```
