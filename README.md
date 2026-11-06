# MHGU

一个仓库，三个部分：**PC 版**、**Android 版**、**Rust 联机模块与 hub 服务器**。游戏（Monster Hunter Generations
Ultimate 1.4.0，Switch）的 32 位 ARM 代码被静态重编译成 C，由同一份宿主运行时在 Vulkan 上运行；Switch 的系统服务与
NVN 图形 API 由运行时重新实现。

**本仓库不包含任何游戏代码、ROM、密钥、存档或 APK。** 游戏代码由玩家用自己的 `exefs`/`romfs` 生成，只在本机使用。

- PC 指南：[docs/pc.md](docs/pc.md)
- Android 指南：[docs/android.md](docs/android.md)（[Español](docs/android.es.md)）
- 联机与 hub 服务器：[docs/online.md](docs/online.md)
- Android 工程笔记：[CLAUDE.md](CLAUDE.md)
- 三个原项目的来源与合并决定：[PROVENANCE.md](PROVENANCE.md)

## 目录

| 路径 | 内容 |
|---|---|
| `runtime/` | 共享 C 运行时：guest 内核/`svc`、HLE 服务、NVN→Vulkan、音频、媒体库、设置菜单、`net/ldn.c` |
| `tools/` | 游戏翻译器（`gen.py`、`lifter.py`、`neon.py`、`nso.py`）、`build_game.py`、PC 构建辅助脚本 |
| `third_party/` | cimgui/ImGui、FSR、volk、VMA、libadrenotools、bcdec、SDL3/shaderc 源码包 |
| `crates/` | Rust：`mhgu-protocol`、`mhgu-link`、`mhgu-link-ffi`（C 库）、`mhgu-hub`（服务器 + Web UI） |
| `Makefile` | PC 构建（Linux；Windows 用 MSYS2） |
| `native/` | Android 的 CMake 与 `build_native.sh`（含 Rust 交叉编译） |
| `android/` | Android 应用（Gradle；`GameActivity`、`GameFolderActivity`、导入界面等） |
| `deps/` | Android 依赖构建脚本（SDL3、shaderc、FFmpeg） |
| `updater/` | PC 自动更新程序 |
| `packaging/` | hub 的 Arch/Windows 打包、systemd unit、示例 Web UI |
| `docs/`、`tests/` | 文档与不依赖游戏文件的测试 |

## PC：构建与运行

依赖：clang/clang++、CMake、Ninja、Rust（cargo）、SDL3、Vulkan loader、shaderc、FFmpeg（系统库即可）。

```sh
# 1) 联机库
cargo build --release -p mhgu-link-ffi        # → target/release/libmhgu_link_ffi.a

# 2) 运行时（GEN 指向你的翻译产物，GAME 指向含 exefs/ 与 romfs/ 的目录）
make -j12 GEN=<gen 目录> GAME=<dump 目录> FFMPEG=system

# 3) 运行
cd build && ./mhgurecomp <游戏目录>
```

只用系统库时可加 `SDL3=system SHADERC=system FFMPEG=system`。翻译游戏代码：`make gen GAME=<dump 目录>`
（需要 `tools/requirements.txt` 里的 Python 包）。

## Android：构建与运行

1. `python tools/build_game.py <含 exefs/ 与 romfs/ 的目录>` → 该目录里生成 `libmhgugame.so` 与 `libmhgugame.id`
   （需要 Python 3、`lz4`；没有 NDK 时脚本会下载 NDK r27）。
2. 先构建依赖（一次性）：`bash deps/build_deps.sh arm64-v8a`、`bash deps/build_ffmpeg.sh arm64-v8a`
   （源码放 `deps/src/`）。已有 `deps/out/arm64-v8a` 可跳过。
3. `bash native/build_native.sh arm64-v8a`：交叉编译 Rust 联机库并构建 `libmain.so`。
4. `cd android && ./gradlew assembleRelease`（签名需 `android/keystore.properties` 与 `mhgu.keystore`）。

公开 APK 不含 `libmhgugame.so`；`-Pwithgame` 仅用于自测，不要发布。细节见 [docs/android.md](docs/android.md)。

## 联机

- **局域网**：主机建房（`runtime/net/ldn.c` 调用 Rust `mhgu-link`），其他玩家“搜索房间”或填 `Join = <IP>:<port>`。
- **互联网**：运行 Rust hub 服务器（`cargo build --release -p mhgu-hub && ./target/release/mhgu-hub -addr :39510`），所有玩家填同一个 `Server` 和 `Matchmaking key`。
- Rust 版 hub 带无依赖 Web UI：`./target/release/mhgu-hub -addr :39510 -web :39511 -web-token <token>`。
- **与模拟器联机（Crossplay）**：`crates/mhgu-crossplay` 实现 Ryujinx 的 RyuLdn 协议与 suyu 房间协议（客户端、房间服务器、
  RyuLdn 服务器和混合 hub），设置菜单 Network 页选择 `Crossplay = Ryujinx` 或 `Room`。可以直接连公共/自建服务器，
  也可以勾选 `RyuHost` / `RoomHost` 在本机内嵌服务器；`RyuServers`、`RoomServer`、`RyuPassword`、`RoomName` 等
  键与菜单项同名。详见 [docs/online.md](docs/online.md#crossplayryujinx--suyu)。

Android 运行时同样链接 `mhgu-link-ffi`（`build_native.sh` 自动交叉编译），协议与 PC 版一致。详见
[docs/online.md](docs/online.md)。

## 测试

```sh
make test-settings                    # 设置 INI 往返、环境变量、live 设置、重启分类
make test-settings-locale             # 菜单翻译覆盖（英/西/中）
cargo test --workspace                # 协议、link、hub、FFI
```

## 许可

MIT（见 `LICENSE`）。第三方组件及许可见 `THIRD_PARTY_NOTICES`。运行时的内核、IPC、NVN 对象与 Maxwell 着色器翻译
派生自 LAS Recomp（MIT）；Android 额外包含 volk、libadrenotools、bcdec 等（各自许可）。
