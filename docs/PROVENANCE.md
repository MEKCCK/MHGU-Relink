# MHGU Relink：来源与合并记录

本仓库由两个有历史的项目合并，并在合并后的树上继续开发 PC/Android 运行时与 Rust 联机层。

| 原项目 | 版本 | 合并角色 |
|---|---|---|
| MHGURecomp | `master` @ `d04f739`（2026-11-03，371 个提交；morerokk 上游快照，MEKCCK fork） | git 历史根；PC 构建（Makefile）、`updater/`、`tests/`、`extras/`、`third_party` 源码包 |
| gu-native | `main` @ `a62b1c6`（2026-11-02，13 个提交） | Android 主干：触摸/Android 特性、`android/`、`native/`、Android 字体与许可 |

## 合并方式

1. **运行时合并**：以 `gu-native` 为 ours、`MHGURecomp` 为共同祖先，逐文件合并，保留双方的特性：
   - Android：触摸控制、haptics、padmap、gamelib_lse、perfstats、DLC、FSR 4、低端/Toaster 模式、
     Adreno/PowerVR 处理、GPU 驱动、Wi-Fi 组播锁与逐网卡搜索。
   - PC：engine modules、hatestep、ShareUrgentCredit、SaveLocation、menu_chord、
     cheats_add/cheats_rescan、VK_EXT_depth_bias_control、romfs 大小写检查、CJK 字体合并、
     overlay_wants_text / 纹理描述符耗尽重试。
2. **配置**：`Config` 取双方字段的并集；`CONFIG_DEFAULT_VALUES` 按最终字段顺序重写；Android 与 PC 的
   `ui_request_import` 语义按平台分开（Android：金手指/模组/DLC；PC：游戏文件/游戏目录等）。
3. **联机**：`runtime/net/ldn.c` 的传输层全部改为 Rust（`mhgu-link-ffi`）：主机/加入/中继、UDP/TCP 链路、
   轮询、踢人与 NetworkInfo 广播。C 侧只保留 `nn::ldn` 的 HLE 状态、局域网扫描和 hub 列表控制协议。
4. **Crossplay**：`crates/mhgu-crossplay` 由上游 suyu/MHGURecomp 0.15 的 RyuLdn / suyu 房间 / 混合 hub
   移植为 Rust；C 侧按同一状态机接入，不再保留这些 C 模块。
5. **构建**：PC 使用根目录 `Makefile`；Android 使用 `native/build_native.sh` + `android/` Gradle 工程，
   两边的 Rust 静态库都在构建时交叉编译并链接。

## 上游 Android 主线的许可证变更

本仓库的 Android 主干来自 `gu-native` 0.1.5 的 MIT 快照（`LICENSE` 版权为 Rokk）。上游在 0.1.7 把项目改成
“All rights reserved”：禁止再分发源码或成品，禁止发布修改版，商业使用也需书面许可；0.1.6 及以前仍为 MIT。

我们不认同、并明确谴责这种做法：项目在 MIT 下接收社区协作、被下游合并之后，单方面把后续版本锁成禁止分发的
专有许可，会破坏开源协作的可预期性，也让下游无法继续基于新版本工作。许可条款可以改，但不该把已经形成的协作
当成单向通道。

因此本项目的立场是：

- 只基于 0.1.5/0.1.6 的 MIT 快照合并，不采用 0.1.7 及以后的代码；
- 本仓库自己的新增代码继续使用 MIT；
- 不接受把 0.1.7+ 代码搬进来的提交，也不接受去掉来源/署名的搬运；
- 欢迎基于证据的来源讨论，但不接受人身攻击或无证据的指控。

## 验证

合并后通过：

- `make test-settings`、`make test-settings-locale`
- `cargo test --workspace`（协议、link、hub、crossplay、RyuLdn/房间服务器）
- PC 构建与启动、Android arm64 交叉编译
- Crossplay：RyuLdn 客户端/服务器、suyu 房间双进程、混合 hub 端到端测试

## 版本与命名

自 **0.2.0** 起，本项目使用自己的版本序列（`VERSION`、Android `versionName`/`versionCode`、
Arch `pkgver`），不再跟随 MHGURecomp 或 gu-native 的 0.1.x 编号。产品标识同时统一为仓库名：

- Android：`com.mhgu.relink`（旧安装 `com.mhgu.recomp` 不能覆盖升级）；
- PC：可执行文件/Arch 包/启动器 `mhgu-relink`、更新器 `mhgu-relink-updater`；
- 兼容保留：数据目录 `MHGURecomp` 与设置文件 `mhgurecomp.ini` 不改名，已有存档和设置继续有效。
