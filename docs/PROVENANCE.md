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

## 验证

合并后通过：

- `make test-settings`、`make test-settings-locale`
- `cargo test --workspace`（协议、link、hub、crossplay、RyuLdn/房间服务器）
- PC 构建与启动、Android arm64 交叉编译
- Crossplay：RyuLdn 客户端/服务器、suyu 房间双进程、混合 hub 端到端测试
