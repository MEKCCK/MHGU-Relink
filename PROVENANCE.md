# 来源与合并记录

本仓库由三个本地项目合并而成。原目录未改动。

| 原项目 | 路径 | 版本/状态 | 合并角色 |
|---|---|---|---|
| MHGURecomp | ` MHGURecomp` | `master` @ `d04f739`（2026-11-03，371 个提交；morerokk 上游快照，MEKCCK fork） | git 历史根；PC 构建（Makefile）、`updater/`、`tests/`、`extras/`、`docs/`、`third_party` 源码包、`.github` |
| gu-native | ` gu-native` | `main` @ `a62b1c6`（2026-11-02，13 个提交） | Android 主干：`runtime/` 的触摸/Android 特性、`android/`、`native/`、`deps/`、Android 字体与许可 |
| mhgu-linux | ` mhgu-linux` | 无 git；快照 2026-11-05/06 | Rust 联机（`crates/`）、更新后的 PC 运行时副本（`base/`：engine modules、`hatestep`、SaveLocation、Rust `ldn.c`、CJK 字体合并）、打包 |

git 历史：以 MHGURecomp 的历史为根，`git merge -s ours` 方式并入 gu-native 的 13 个提交（保留可达性，树由合并结果重写）。mhgu-linux 无 git，来源记录在此文件。

## 合并方式

1. **运行时三分合并**：对每个文件以 `gu-native` 为 ours、`MHGURecomp` 为共同祖先、`mhgu-linux/base` 为 theirs
   （`git merge-file --diff3`，先统一 LF）。自动合并 107 个文件；16 个文件、45 处冲突逐一解决。
2. **取并集，而不是二选一**：
   - 保留 gu-native 的 Android 特性：触摸控制、`haptics`、`padmap`、`gamelib_lse`、`perfstats`、`weapon`、DLC、
     FSR 4、低端/Toaster 模式、Adreno/PowerVR 处理、Android GPU 驱动、Wi-Fi 组播锁与逐网卡搜索。
   - 保留 mhgu-linux/base 的 PC 新特性：engine modules、`hatestep`、`ShareUrgentCredit`、`SaveLocation`、
     `menu_chord`、`cheats_add`/`cheats_rescan`、`VK_EXT_depth_bias_control`、romfs 大小写检查、CJK 字体合并、
     `overlay_wants_text`/纹理描述符耗尽重试。
3. **配置**：`Config` 为两侧字段的并集，`CONFIG_DEFAULT_VALUES` 按最终字段顺序重写（PC 字段追加在
   `touch_combo_pad` 之后）；`share_urgent` 恢复；Android 与 PC 的 `ui_request_import` 语义按平台分开
   （Android：0 金手指 / 1 模组 / 2 DLC；PC：1 游戏文件 … 7 游戏目录）。
4. **联机统一**：`runtime/net/ldn.c` 的传输层全部使用 `mhgu-link-ffi`（Rust）：主机/加入/中继、UDP/TCP 链路、
   轮询、踢人（`mhgu_ldn_reject`）与 NetworkInfo 广播（`mhgu_ldn_update_info`）。C 侧只保留 `nn::ldn` 的 HLE
   状态与结构、局域网扫描（Android 的组播锁、逐网卡探测）以及 hub 列表/检查的控制协议。旧的 C host/station
   线程、socket 传输与 relay 已删除；Android 构建脚本交叉编译该静态库并链接。
5. **构建统一**：
   - PC：根目录 `Makefile`（原 `base/Makefile`，去掉 `PLATFORM=android` 分支，Rust 路径改为 `target/`、`crates/`）。
   - Android：`native/CMakeLists.txt` + `android/`（原 gu-native），新增 Rust 库与 `mhgu_link.h` 链接。
   - 删除重复的 `base/`、recomp 的 `android/android.mk`/`apk.mk` 构建路径与 `run-*.bat` 之外的旧入口。
6. **测试**：
   - `tests/test_settings.c` 与 `tests/test_settings_locale.py` 保留并适配（HP 改为 live、FSR 4 设置、
     `SyncSmallMonsters` 默认 1、手机帮助表不计入 locale 覆盖率、补 PC 新增文案翻译）。
   - 删除 `tests/test_settings_panel.cpp`：它校验的是旧 PC 菜单的逐行导航顺序，而本仓库采用 gu-native 的
     触摸优先菜单（行为契约已不存在，按“不重钉旧实现”的原则删除）。
7. **移除 Go 版 hub（`server/`）**：`crates/mhgu-hub`（Rust）已与其线格式逐字节兼容，并带无依赖 Web UI
   （`/api/hubs`、管理页、封禁、日志）；保留两份实现只会产生第二个协议源。运行时、文档、打包现在只指向
   `mhgu-hub`，仓库不再包含 Go 代码。
8. **文档**：三份 README 分别移入 `docs/pc.md`、`docs/android.md`（+ `android.es.md`）、`docs/online.md`；
   根 `README.md` 重写为统一索引；`CLAUDE.md` 增补合并后的目录与构建说明。

## 验证记录（合并时）

- `make -j12 FFMPEG=system`：生成 `build/mhgurecomp`、`build/mhgurecomp-updater`、`build/mhgugame.so`。
- `build/mhgurecomp` 在无游戏文件的目录下运行：按预期报错退出（未加载游戏）。
- `make test-settings`、`make test-settings-locale`：通过。
- `cargo test --workspace`：通过。
- `bash native/build_native.sh arm64-v8a`（NDK 27，`deps/out/arm64-v8a`）：生成 `libmain.so` 并复制到 `jniLibs`；
  Rust 交叉编译通过。未在真机上运行。


## 上游同步（2026-11-07）

上游 `morerokk/MHGURecomp` 已到 `ce0ce81`（0.15.1，标签到 0.15.1）。本仓库以 `682e8ed`（0.14.1 附近，
即本仓库基线最匹配的提交）为有效祖先，合入其后 31 个提交的运行时/工具变更：

- 设置：默认值改为 designated initializer，live 变量按 ini 分节；`LoadingIndicator`（仅 ini）。
- 启动：加载指示器（`overlay_init_early`/`overlay_loading`，提前初始化渲染器），`forge_imgui` 早期字体。
- 性能：解释器基本块（`interp.c`）、渲染线程加速（AVX2 索引扫描、缓存 getenv/计时器）、FXAA 改写。
- 修复：手柄震动锁输入、Android 最小化后黑屏/Mali 崩溃（surface 生命周期）、侧键绑定、16:10 铁匠铺预览、
  小怪同步、`emsync`、作弊热重载、引擎模块修正（`emstall.c`）。
- 工具：`tools/gen.py` 等更新 → **GAMELIB_ID 改变**（Android/PC 都需要重新生成/编译游戏代码）。

**未合入：crossplay（C）**。上游新增的 `runtime/net/{room,roomsrv,ryuldn,ryusrv,xbridge}.*`、
`third_party/enet` 及相关测试是使用 C/C++ 与 ENet 的第二套联机栈（Ryujinx/suyu 房间）。按本仓库
“Rust 联机模块为唯一传输层”的决定，这些文件不进入本树；Rust 侧已按上游行为更新：

- `LINK_PROTO_VERSION` 2 → 3（`mhgu-protocol`；C 侧 `PROTO_VERSION` 同步为 3，否则 LAN/hub 列表不匹配）。
- `mhgu-hub`：`-min-proto N`（拒绝旧协议，默认 0）、`-separate-phones[=false]`（Android 公共房间并入公共房间）、
  被拒协议的日志、房间名日志。
- 相关 Rust 测试更新到协议 3。

因此当前树**不支持**与 Ryujinx/suyu 房间的 crossplay；“普通联机 + Rust hub”与上游协议 v3 客户端兼容。
