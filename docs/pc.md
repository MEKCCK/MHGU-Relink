# MHGU Relink：PC 版

Monster Hunter Generations Ultimate（Switch 1.4.0）的静态重编译 PC 版。游戏代码由玩家自己的
`exefs` / `romfs` 生成，仓库不附带游戏文件。

## 特性

- Vulkan 渲染；分辨率、MSAA、FXAA、超采样、VSync 可调
- 任意宽屏比例（16:10、16:9 及更宽）、30–240 FPS 上限
- 支持常见手柄，实验性键鼠
- 局域网、互联网 hub；Ryujinx / suyu 模拟器 Crossplay（含混合 hub）
- 模组（romfs / exefs 补丁 / Forge 插件）与金手指
- 自动更新（正式发行版）

## 需要什么

- Linux x86-64（Arch 包）或 Windows（MSYS2 构建）
- Vulkan 1.2 驱动
- 游戏 1.4.0 的 `exefs` 与已合并更新的 `romfs`
- 第一次生成游戏代码需要 PC；游戏代码只在本机使用

## 安装（Arch Linux）

```sh
sudo pacman -U mhgu-recomp-0.1.6-3-x86_64.pkg.tar.zst
mhgu-recomp /path/to/MHGU-1.4
```

启动器会依次查找 `$HOME/game/MHGU-1.4`、`$HOME/game/MHGU` 等常见目录；也可以设 `MHGU_GAME_DIR`。
游戏目录必须包含 `exefs/`、`romfs/`，以及 **x86-64** 的 `libmhgugame.so`（Android 的 ARM 库不能用于 PC）。

状态、设置、缓存默认在 `$XDG_DATA_HOME/MHGURecomp`（通常是 `~/.local/share/MHGURecomp`）：

| 内容 | 路径 |
|---|---|
| 设置 | `mhgurecomp.ini`（菜单是 F1） |
| 存档 | `save/save_0/` |
| 日志 | `log.txt` |
| 模组 / 金手指 | `mods/`、`cheats/` |
| 着色器缓存 | `cache/` |

## 从源码构建

依赖：clang/clang++、CMake、Ninja、Rust、SDL3、Vulkan loader、shaderc、FFmpeg（也可用项目内置构建）。

```sh
cargo build --release -p mhgu-link-ffi -p mhgu-crossplay
make gen GAME=/path/to/MHGU-1.4          # 翻译游戏代码到 generated/
make -j12 GEN=generated GAME=/path/to/MHGU-1.4 FFMPEG=system
cd build && ./mhgurecomp /path/to/MHGU-1.4
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
| `libmhgugame.so: cannot open shared object file` | 游戏目录里放 PC x86-64 库，不要用 Android ARM 库 |
| `libmhgugame.so is from another version` | 用当前仓库的 `make gen` / 工具重新生成游戏代码 |
| 找不到游戏文件 | 传入的目录必须直接包含 `exefs/` 与 `romfs/` |
| 启动黑屏 / 崩溃 | 查看 `log.txt`；确认 Vulkan 驱动与游戏文件完整 |
| hub 连不上 | 检查网络页提示；自建服务器需可达端口 |

## 许可

MIT；第三方组件见 `THIRD_PARTY_NOTICES`。运行时的内核、IPC、NVN 与着色器翻译派生自 LAS Recomp（MIT）。
