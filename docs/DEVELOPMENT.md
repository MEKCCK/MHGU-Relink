# MHGU Relink：开发说明

一个仓库同时构建 PC、Android 和 Rust 联机层。本文给继续开发的人；玩家安装见
[pc.md](pc.md) / [android.md](android.md)，联机见 [online.md](online.md)。

## 结构

| 路径 | 内容 |
|---|---|
| `runtime/` | C 运行时：guest 内核/`svc`、HLE、NVN→Vulkan、音频、媒体库、设置菜单 |
| `runtime/net/ldn.c` | `nn::ldn` 的 HLE；按 `Crossplay` 模式转给 Rust 模块 |
| `tools/` | `gen.py`（函数发现/代码生成）、`lifter.py`（A32/VFP）、`neon.py`、`nso.py`、`build_game.py` |
| `crates/mhgu-protocol` | MHGS/MHGR/MHGL/MHGD 线格式、`VPacket`、`NetworkInfo` |
| `crates/mhgu-link` / `-ffi` | 局域网/互联网 link 客户端与 C 库 |
| `crates/mhgu-hub` | 互联网 hub 服务器 + Web UI |
| `crates/mhgu-crossplay` | RyuLdn、suyu 房间、混合 hub 与 C FFI |
| `native/` | Android CMake、`build_native.sh`（含 Rust 交叉编译） |
| `android/` | Gradle 应用、游戏文件夹选择、触屏 |
| `deps/` | Android SDL3 / shaderc / FFmpeg 依赖构建脚本 |

## 约定

- 绝不提交或发布 `exefs`、`romfs`、`generated/`、`libmhgugame.so`、`libmhgugame.id`、存档、签名密钥。
- APK / 发布包不得包含游戏代码；每个玩家用自己的 `tools/build_game.py` 生成。
- 保留 `LICENSE`、`THIRD_PARTY_NOTICES`。
- 新菜单、提示、触屏控件必须沿用游戏的青铜/金色/酒红风格。
- 不擅自降低玩家的画质或音频设置。

## 构建

### PC

```sh
cargo build --release -p mhgu-link-ffi -p mhgu-crossplay
make gen GAME=<dump>
make -j12 GEN=generated GAME=<dump> FFMPEG=system
```

`make` 把运行时和游戏库放进 `build/`；`GEN` 默认 `generated/`。可用 `SDL3=system SHADERC=system
FFMPEG=system` 链接系统库。

### Android

```sh
# 一次性：依赖（源码在 deps/src，产物在 deps/out；现有 deps/out 可跳过）
# build_deps.sh / build_ffmpeg.sh 按 Windows/MSYS2 的 SDK 路径编写；Linux 上复用已有产物即可
bash deps/build_deps.sh arm64-v8a
bash deps/build_ffmpeg.sh arm64-v8a

# 生成/交叉编译
python3 tools/build_game.py <dump>
bash native/build_native.sh arm64-v8a

# APK（JDK 17；签名读 android/keystore.properties）
cd android && JAVA_HOME=/usr/lib/jvm/java-17-openjdk ./gradlew assembleRelease
```

`build_native.sh` 会计算 `GAMELIB_ID`（工具、HLE 符号、CPU/NEON 头与编译参数），写入
`android/app/src/main/assets/gamelib_id.txt`，APK 用它检查设备上的 `libmhgugame.id`。任何会改变这些输入的提交都必须
重新生成游戏代码，否则设备会报 `from another version`。

## 联机架构

- `runtime/net/ldn.c` 保留 `nn::ldn` 状态、扫描和结构体；传输由 Rust 完成。
- `mhgu-link`：原 MHGL/MHGD 局域网和 hub 中继。
- `mhgu-crossplay/ryu.rs`：RyuLdn 客户端；`ryu_server.rs`：RyuLdn 主服务器。
- `room.rs` / `roomsrv.rs`：suyu 房间客户端与服务器（ENet、zstd）。
- `bridge.rs`：主机在 RyuLdn hub 上同时接纳房间玩家，做 IP/MAC 改写；`Crossplay = Mixed`。

C API 在 `crates/mhgu-crossplay/src/ffi.rs`，由 `runtime/net/ldn.c` 直接声明调用；PC 与 Android 都链接
`libmhgu_crossplay.a`。

## 测试

```sh
make test-settings                 # INI、环境变量、live 设置、重启分类
make test-settings-locale          # 英/西/中菜单翻译覆盖
cargo test --workspace             # 协议、link、hub、crossplay、RyuLdn/房间服务器
cargo test -p mhgu-crossplay --test bridge   # 混合 hub
```

Rust 测试不需要游戏文件。

## 调试

- `MHGU_LOG_NET=1`：`ldn`、hub、Crossplay 日志；`=2` 还记录 HLE 调用。
- `MHGU_LOG_MOVES`、`MHGU_QUICKHUNT`、`MHGU_EMTRACE`、`MHGU_SAMPLE` 等见源码注释。
- Android：`adb logcat -s MHGU`，或 `Android/data/com.mhgu.recomp/files/log.txt`。
- PC：游戏目录/状态目录的 `log.txt`。

## 发布

```sh
make dist VERSION=x.y.z
cd packaging/arch-game && makepkg -f
cd packaging/arch && makepkg -f
```

Arch 包的 `pkgver` 要跟 `VERSION` 一致。PKG/APK 都不包含游戏代码。
