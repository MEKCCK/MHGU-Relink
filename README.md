# MHGU Relink

**Monster Hunter Generations Ultimate（Switch 1.4.0）的 PC / Android 原生重编译版，加上 Rust 联机与模拟器 Crossplay。**

游戏原本的 32 位 ARM 代码由本项目静态翻译成 C，再由同一份宿主运行时在 Vulkan 上执行；Switch 的系统服务、
NVN 图形 API、音频与媒体库都由运行时重新实现；PC/Android 均支持原生 HDR 输出。联机层用 Rust 重写，支持局域网、互联网 hub，以及和
Ryujinx / suyu 模拟器互通。

**本仓库不包含任何游戏代码、ROM、密钥、存档或 APK。** 游戏代码由玩家用自己的 `exefs` / `romfs` 在本机生成。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/pc.md](docs/pc.md) | PC 版安装、控制、设置、模组、从源码构建 |
| [docs/android.md](docs/android.md) | Android 安装、游戏文件、存档导入导出、排错 |
| [docs/online.md](docs/online.md) | 局域网、hub 服务器、Ryujinx / suyu Crossplay、混合 hub |
| [docs/CROSSPLAY-zh-CN.md](docs/CROSSPLAY-zh-CN.md) | Crossplay 选项与端口速查 |
| [docs/HDR.md](docs/HDR.md) | 原生 HDR 输出与 HDR 分析 |
| [docs/MODS-zh-CN.md](docs/MODS-zh-CN.md) | Android 模组导入格式与兼容范围 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 项目结构、构建、测试与开发约定 |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | 两个原项目的合并来源与验证记录 |

## 目录

| 路径 | 内容 |
|---|---|
| `runtime/` | 共享 C 运行时：guest 内核/`svc`、HLE、NVN→Vulkan、音频、媒体库、设置菜单、`net/ldn.c` |
| `tools/` | 游戏翻译器（`gen.py`、`lifter.py`、`neon.py`、`nso.py`）、`build_game.py` |
| `crates/mhgu-link*` | Rust 联机核心：协议、link 客户端、C FFI、hub 服务器 |
| `crates/mhgu-crossplay` | RyuLdn 客户端/服务器、suyu 房间客户端/服务器、混合 hub、C FFI |
| `crates/mhgu-hub` | 互联网 hub 服务器 + 无依赖 Web UI |
| `native/`、`android/` | Android CMake/NDK 构建与 Gradle 工程 |
| `deps/` | Android 依赖构建脚本（SDL3、shaderc、FFmpeg） |
| `updater/`、`packaging/` | PC 自动更新、Arch/Windows 打包与 systemd unit |
| `tests/` | 不需要游戏文件即可运行的测试 |

## 快速开始（PC）

```sh
# 1) Rust 库
cargo build --release -p mhgu-link-ffi -p mhgu-crossplay

# 2) 运行时（GEN=翻译产物，GAME=含 exefs/ 与 romfs/ 的目录）
make -j12 GEN=<gen> GAME=<dump> FFMPEG=system

# 3) 运行
cd build && ./mhgurecomp <游戏目录>
```

生成游戏代码与完整依赖说明见 [docs/pc.md](docs/pc.md)。

## 快速开始（Android）

```sh
python3 tools/build_game.py <含 exefs/ 与 romfs/ 的目录>
bash native/build_native.sh arm64-v8a
cd android && JAVA_HOME=/usr/lib/jvm/java-17-openjdk ./gradlew assembleRelease
```

Android APK 不包含游戏代码。安装后请在所选游戏文件夹中提供与 APK 版本匹配的 `libmhgugame.so` 和
`libmhgugame.id`。详见 [docs/android.md](docs/android.md)。

## 联机

- **局域网 / 互联网 hub**：游戏内创建或搜索本地通信房间；hub 服务器用 `target/release/mhgu-hub`。
- **模拟器 Crossplay**：设置菜单选择 `Crossplay = Ryujinx / Room / Mixed`，连接 RyuLdn 服务器、suyu 房间服务器，
  或两者同时使用。详见 [docs/online.md](docs/online.md#crossplayryujinx--suyu)。
- 公共测试服务器：`114.66.25.224`（RyuLdn `30456/tcp`，suyu 房间 `24872/udp`，hub `39510/tcp`）。

## 测试

```sh
make test-settings
make test-settings-locale
cargo test --workspace
```

## 许可

MIT，见 `LICENSE`；第三方组件许可见 `THIRD_PARTY_NOTICES`。运行时的内核、IPC、NVN 对象与 Maxwell
着色器翻译派生自 LAS Recomp（MIT）；Android 另含 volk、libadrenotools、bcdec 等，各自保留许可。
