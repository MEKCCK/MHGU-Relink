# MHGU Relink：PC 版

Monster Hunter Generations Ultimate（Switch 1.4.0）的静态重编译 PC 版。游戏代码由玩家自己的
`exefs` / `romfs` 生成，仓库不附带游戏文件。

## 特性

- Vulkan 渲染；分辨率、MSAA、FXAA、超采样、VSync 可调
- 任意宽屏比例（16:10、16:9 及更宽）、30–240 FPS 上限
- 支持常见手柄，实验性键鼠
- 局域网、互联网 hub；Ryujinx / suyu 模拟器 Crossplay（含混合 hub）
- 模组（romfs / exefs 补丁 / Forge 插件）与金手指
- 单人游玩时缩短与加载无关的过渡等待（任务出发、离镇、换区），联机时保持原版节奏；`MHGU_NO_FASTLOAD=1` 还原游戏原始计时
- 自动更新（正式发行版）

## 需要什么

- Linux x86-64（Arch 包）或 Windows（MSYS2 构建）
- Vulkan 1.2 驱动
- 游戏 1.4.0 的 `exefs` 与已合并更新的 `romfs`
- 第一次生成游戏代码需要 PC；游戏代码只在本机使用

## 安装（Arch Linux）

```sh
sudo pacman -U mhgu-relink-0.2.0-1-x86_64.pkg.tar.zst
mhgu-relink /path/to/MHGU-1.4
```

启动器会依次查找 `$HOME/game/MHGU-1.4`、`$HOME/game/MHGU` 等常见目录；也可以设 `MHGU_GAME_DIR`。
游戏目录必须包含 `exefs/`、`romfs/`，以及 **x86-64 的 `mhgugame.so`**（启动器也接受旧名
`libmhgugame.so`，但它必须是 PC 的 x86-64 构建；Android 的 ARM `libmhgugame.so` 不能用于 PC）。

状态、设置、缓存默认在 `$XDG_DATA_HOME/MHGURecomp`（通常是 `~/.local/share/MHGURecomp`）：

| 内容 | 路径 |
|---|---|
| 设置 | `mhgurecomp.ini`（菜单是 F1） |
| 存档 | `save/save_0/` |
| 日志 | `log.txt` |
| 模组 / 金手指 | `mods/`、`cheats/` |
| 着色器缓存 | `cache/` |

## 生成游戏代码（`.so` / `.dll` / `.id`）

游戏代码不在发布包内。它由玩家自己的 `exefs` 翻译并编译成宿主库；PC 运行时的库名与位置如下：

| 平台 | 游戏库 | ID 文件 | 运行时从哪里加载 |
|---|---|---|---|
| Linux | `build/mhgugame.so`（x86-64） | `obj/gamelib.id` | `mhgu-relink` 同目录，或游戏目录（启动器自动设置） |
| Windows | `build/mhgugame.dll`（x86-64） | `obj/gamelib.id`；独立工具写 `mhgugame.id` | `mhgu-relink.exe` 同目录的 `mhgugame.dll` |
| Android | 游戏目录的 `libmhgugame.so`（arm64-v8a） | 游戏目录的 `libmhgugame.id` | APK 选中的游戏目录 |

注意：Linux 的 `mhgugame.so` 和 Android 的 `libmhgugame.so` 只差一个 `lib` 前缀，但前者是 x86-64，
后者是 arm64-v8a，**不能互换，也不能只按文件名判断**。启动器同时接受 `mhgugame.so` 和旧的
`libmhgugame.so` 作为 Linux 库，但文件必须是 PC 构建。

`gamelib.id`（Android 叫 `libmhgugame.id`）是 16 位十六进制字符串，等于编译进库里的 `GAMELIB_ID`。这个值由
`android/app/src/main/assets/gamelib_id.txt` 固定，Makefile、Android 构建和 `build_game_windows.py` 都读它；
运行时/APK 启动时会比较它：不一致就报 `was built for another version`，必须用匹配的 ID 重新生成库。
库本身已经包含这个 ID；`.id` 文件是给启动器、打包和排查用的。

### Linux（PC）

```sh
make gen GAME=/path/to/MHGU-1.4
make -j12 GAME=/path/to/MHGU-1.4 FFMPEG=system
# build/mhgugame.so 和 obj/gamelib.id
```

把 `build/mhgugame.so` 复制到 `build/mhgu-relink` 旁边再运行；只编译运行时不会自动生成游戏代码。

### Windows（PC）

官方方式是在 MSYS2 里执行同一套 Makefile：

```sh
make gen GAME=E:/path/to/MHGU-1.4
make -j12 PLATFORM=windows GAME=E:/path/to/MHGU-1.4
# build/mhgugame.dll 和 obj/gamelib.id
```

需要 MSYS2 clang/lld 以及 SDL3、shaderc、FFmpeg、Vulkan 头。`mhgugame.dll` 必须由**同一份源码和同一套
Windows 编译参数**生成；换一台机器只复制 DLL 而不复制匹配的 `mhgu-relink.exe`，启动时会被 ID 检查拒绝。
Windows 下没有单独的 `.id` 加载逻辑，ID 已经嵌在 DLL 里。

如果只想让玩家自己生成 DLL，用打包的独立工具（见 `tools/build_game_windows.py`）：

```sh
py tools/build_game_windows.py --exe windows/mhgu-relink.exe <含 exefs/romfs 的目录>
# 输出 <目录>/mhgugame.dll 和 <目录>/mhgugame.id
```

工具会读取 `mhgu-relink.exe` 内嵌的 `GAMELIB_ID`（没有 `--exe` 时读共享 ID 文件）并写入 DLL；游戏文件不会被
上传或打包。拿到别人的构建时，用 `--exe` 指向那一份 `mhgu-relink.exe`，不要只按版本号猜 ID。

### Android

```sh
python3 tools/build_game.py <含 exefs/ 与 romfs/ 的目录>
# 输出 <目录>/libmhgugame.so 和 <目录>/libmhgugame.id
```

APK 不自带游戏代码；`libmhgugame.so` / `.id` 必须与当前 APK 的 `gamelib_id.txt` 一致。

## 从源码构建

依赖：clang/clang++、CMake、Ninja、Rust、SDL3、Vulkan loader、shaderc、FFmpeg（也可用项目内置构建）。

```sh
cargo build --release -p mhgu-link-ffi -p mhgu-crossplay
make gen GAME=/path/to/MHGU-1.4          # 翻译游戏代码到 generated/
make -j12 GEN=generated GAME=/path/to/MHGU-1.4 FFMPEG=system
cd build && ./mhgu-relink /path/to/MHGU-1.4
```

常用选项：

- `SDL3=system SHADERC=system FFMPEG=system`：使用系统库
- `make test-lift`：用 Unicorn 差分测试重编译器
- `make test-settings`、`make test-settings-locale`：设置与翻译测试
- `make dist VERSION=x.y.z`：生成发布压缩包

Windows 使用 MSYS2 UCRT64：安装 clang/lld/pkgconf/SDL3/shaderc/nasm/Vulkan 头与 loader，然后执行
`make gen GAME=...` 与 `make -j4 GAME=...`。

## 操作

| 键 | 作用 |
|---|---|
| WSAD | 左摇杆（按住 Left Shift 走路） |
| 鼠标 | 镜头 |
| 左/右键 | X / A |
| 空格 / R / 中键 | B / Y |
| 滚轮 | 道具栏左右 |
| Q / E | L / R |
| Tab / T | ZL / ZR |
| Enter / Backspace | Plus / Minus |
| 方向键 | 十字键 |
| Ctrl / Alt | 左右摇杆按下 |

F1 设置菜单，Alt+Enter / F11 全屏，F7 切换 MSAA，F12 截图。手柄通常即插即用；多手柄时按一下想用的那个。
菜单里可以改键，最多两个键绑一个动作。`/` 在联机大厅打开聊天。

## 设置与联机

设置菜单分显示、画面与音频、操作、网络、其他等页。网络页关键项：

| INI | 菜单 | 说明 |
|---|---|---|
| `Port` | 主机端口 | 自建 hub 的 TCP/UDP 端口，默认 39500 |
| `Join` | 搜索主机 | 额外搜索的 `host[:port]` 列表 |
| `Server` | Hub 服务器 | 互联网 hub；空=默认，`Off`=关闭 |
| `Key` | 匹配密钥 | 相同密钥的玩家互相可见；空=公开 |
| `Crossplay` | Crossplay | `Off` / `Ryujinx` / `Room` / `Mixed` |
| `RyuServers` | RyuLdn 服务器 | `host[:port]` 列表，逗号分隔 |
| `RyuPassword` | RyuLdn 密码短语 | 空=公开 |
| `RoomServer` | suyu 房间服务器 | `host[:port]` |
| `RoomName` / `RoomPassword` | 房间名/密码 | 与模拟器一致 |

完整联机说明见 [online.md](online.md)。

## 模组与金手指

- 模组放进 `mods/`，子目录里可有 `romfs/`、`exefs/`、`nativeNX/`；多个模组按
  `mods/load_order.txt` 顺序加载（后加载覆盖先加载）。F1 的 Mods 页可直接启用/排序/检查冲突。
- `.pchtxt` / `.ips` 补丁会被解释执行；Forge 插件放 `romfs/nativeNX/plugins/`。
- 金手指放 `cheats/*.txt`，F1 的 Cheats 页启停；文件名以 `_` 开头可停用但不删除。
- 联机使用金手指前请告知其他玩家。

## 常见问题

| 问题 | 处理 |
|---|---|
| `mhgugame.so: cannot open shared object file` | 游戏目录里放 PC x86-64 的 `mhgugame.so`，不要用 Android 的 ARM `libmhgugame.so` |
| `mhgugame.so is from another version` | 用当前仓库的 `make gen` / 工具重新生成游戏代码 |
| 找不到游戏文件 | 传入的目录必须直接包含 `exefs/` 与 `romfs/` |
| 启动黑屏 / 崩溃 | 查看 `log.txt`；确认 Vulkan 驱动与游戏文件完整 |
| hub 连不上 | 检查网络页提示；自建服务器需可达端口 |

## 许可

MIT；第三方组件见 `THIRD_PARTY_NOTICES`。运行时的内核、IPC、NVN 与着色器翻译派生自 LAS Recomp（MIT）。


## 游戏内语音（Voice）

设置菜单的 **语音聊天** 支持 `Off / Always / Push to talk`：Opus 48 kHz 单声道，10 ms 一帧，24 kbps，
走联机层的 `F_AUX`；Crossplay 时走本地虚拟网络的 UDP `47319`，不会进入游戏自己的 socket。
双方都必须是本项目（或同一套语音协议）的客户端；原版 Switch 不解析该协议。

- `MicVolume` / `VoiceVolume`：麦克风与对方语音音量；`VoiceVolume` 默认 70，内部有 2 倍增益。
- `NoiseSuppression`：更严格的麦克风门限（并非 RNNoise）。
- Push to talk：键盘 `V`，或手柄的 MISC1 按钮。
- PC 构建需要 `libopus`（Arch：`pacman -S opus`）；没有它时构建仍可完成，但语音关闭。
