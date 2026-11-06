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
4. **联机统一**：`runtime/net/ldn.c` 使用 `mhgu-link-ffi`（Rust）作为主机/加入/中继传输；Android 构建脚本
   交叉编译该静态库并链接（LAN 扫描、组播锁、逐网卡探测仍由 C 侧保留）。
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
