# MHGU Relink：Android 版

Monster Hunter Generations Ultimate（Switch 1.4.0）的 Android 原生重编译版。与 PC 版共用 `runtime/`，
支持触屏、手柄、局域网 / 互联网联机，以及 Ryujinx / suyu Crossplay。

**APK 不包含游戏代码。** 游戏代码由玩家用自己的 `exefs` / `romfs` 在 PC 上生成。

## 需要什么

- Android 9 或更新（9 为实验性），64 位 ARM，Vulkan 1.2；近几年中高端设备即可
- 设备上约 13 GB 空闲空间
- 一台 Windows / Linux PC 用于生成游戏代码
- 自己拥有且已更新到 1.4.0 的游戏文件

低端设备可在设置里打开 **低端模式**：3D 分辨率 40%、30 FPS，再按需调整。

## 第一步：准备游戏文件

需要 `exefs/` 和 `romfs/`，且已包含 1.4.0 更新。放在同一个文件夹：

```text
MHGU/
├─ exefs/
│  ├─ main
│  ├─ main.npdm
│  ├─ rtld
│  ├─ sdk
│  └─ subsdk0
└─ romfs/
   ├─ nativeNX/
   └─ ...（约 35,531 个文件）
```

如果游戏在 Ryujinx 里：确认已应用 1.4.0 更新，右键游戏 → **Extract Data**，分别导出 **ExeFS** 和
**RomFS** 到同一个空文件夹。本项目不提供游戏文件、密钥，也不说明如何从主机 dump。

## 第二步：生成游戏代码

```sh
python3 tools/build_game.py /path/to/MHGU
```

脚本会读取你自己的文件，把 `exefs` 翻译并编译成 ARM64 的 `libmhgugame.so` 和 `libmhgugame.id`，放回同一
文件夹。APK 不内置这两个文件，首次启动时会从你选择的游戏文件夹加载。首次运行会下载 Android NDK r27（约 1.8 GB，缓存在 `~/.mhgu-tool`）；可用 `--ndk <路径>` 指定已有 NDK。
生成过程约几分钟，完成后不要分享这两个文件。

## 第三步：安装 APK

1. 把整个 `MHGU/` 文件夹复制到手机（数据线、SD 卡均可），**不要**放进 `Android/data`。
2. 安装 APK。首次需要允许浏览器/文件管理器安装应用。
3. 打开 **MHGU Relink**，点 **Give permission and choose folder**，允许文件访问。
4. 选择包含 `exefs/`、`romfs/`、`libmhgugame.so` 和 `libmhgugame.id` 的 `MHGU` 文件夹，点 **Use this folder**。
5. 校验通过后游戏启动。

游戏只读取该文件夹；存档、设置、日志默认在 `Android/data/com.mhgu.recomp/files`。换游戏文件夹：
齿轮 → **Menu Options** → **Game folder**。

## 应用文件文件夹

`Android/data` 在 Android 11 及以后无法用手机上的文件管理器打开，所以应用可以把**自己的文件**（设置
`mhgurecomp.ini`、存档、模组、金手指、截图、`log.txt`）放到你选择的文件夹里：

- 首次启动（以及重装后，重装会忘记这个选择）会询问文件放在哪里；也可以稍后在
  齿轮 → **Menu Options** → **Files folder** 里选择。重装后再次选择以前用过的文件夹即可继续使用其中的文件。
- 选择后应用会把文件复制过去（已存在的文件保留；如果那里的文件比应用里的旧，例如在另一个文件夹玩过之后
  切回来，会先询问是否替换，被替换的文件保留为 `.bak`），在 `files-folder.txt`（应用自己的文件夹中）记下
  路径，然后关闭；重新打开即可。游戏文件（`data/`）不会被复制，仍在原位置使用；着色器缓存会重新生成。
- 应用需要 Android 的“所有文件访问权限”才能读写该文件夹；选择时系统会请求它。此功能需要 Android 11
  或更新；Android 10 及更早仍使用应用自己的文件夹。
- **Use the app's folder** 把文件复制回应用自己的文件夹，并关闭应用；所选文件夹中的文件保持原样。
- 如果所选文件夹在启动时不可用（权限关闭、文件夹被移动或删除、U 盘/SD 卡不在），启动时会说明，并让你
  重新选择或本次使用应用自己的文件夹；如果它已永久消失，可在同一设置页忘记它。
- `log.txt` 会写在该文件夹里；**Share log** 也从那里读取。

## 存档

### 导入

1. 找到存档文件 `system`（Ryujinx：右键游戏 → Open User Save Directory；yuzu：Open Save Data Location）。
2. 复制到手机（如下载目录）。
3. 游戏内：齿轮 → **Menu Options** → **Import save**，选择该文件。游戏会重启；旧存档备份在
   `Android/data/com.mhgu.recomp/files/MHGURecomp/save_backups`（如果选了应用文件文件夹，则在其中的
   `MHGURecomp/save_backups`）。

### 导出

1. 游戏内存档后，齿轮 → **Menu Options** → **Export save**，选择目标文件夹。
2. 生成 `MHGU_save_.../`，包含 `system` 与 `system_backup`。
3. 把这几个文件复制到 Ryujinx / yuzu 的用户存档目录。

## 联机

- **局域网 / hub**：游戏内本地通信创建/搜索房间；与 PC 版同一套协议。
- **Crossplay**：齿轮 → **Network**：
  - `Crossplay = Ryujinx`：填 `RyuServers`，连 RyuLdn 服务器
  - `Crossplay = Room`：填 `RoomServer`，连 suyu 房间
  - `Crossplay = Mixed`：同时填两者，房间玩家会由主机桥接进 RyuLdn hub
- 公共测试服务器：RyuLdn `114.66.25.224:30456`，suyu 房间 `114.66.25.224:24872`，hub `114.66.25.224:39510`。
- 所有人游戏版本必须是 1.4.0；密码短语/房间密码要一致。

详见 [online.md](online.md)。

## 手柄与触屏

触屏提供移动、镜头、锁怪、道具、攻击、闪避等布局；设置里可改透明度、大小、震动和自定义布局。
蓝牙/USB 手柄通常直接可用，多手柄时按一下想用的那个。三星设备可用 adb 安装以获得 Game Booster：

```sh
adb install -r -i com.mhgu.recomp MHGU-0.1.6.apk
```

## 排错

| 问题 | 处理 |
|---|---|
| `python` 找不到 | 重装 Python 并勾选 Add to PATH，重开终端 |
| `there is no exefs/main` | 路径必须是直接包含 `exefs/`、`romfs/` 的文件夹 |
| `exefs is not version 1.4.0` | 使用已合并 1.4.0 更新的 exefs |
| `romfs is incomplete` | romfs 必须有 35,531 个文件；重新完整复制 |
| `libmhgugame.so is missing` | 先在 PC 执行第二步，再把生成的 `.so` 与 `.id` 复制到手机文件夹 |
| `libmhgugame.so is from another version` | 用当前版本仓库重新生成（`.id` 必须与 APK 一致） |
| 游戏自己关闭 | 关闭后台应用、降低 3D 分辨率；通常是内存不足 |
| 只有触屏没有画面 | romfs 复制不完整，重新复制并确认约 12.3 GB |
| 弱机卡顿 | 低端模式、关阴影、3D 分辨率 33% / 25% |

仍然失败时，用齿轮 → **Menu Options** → **Share log** 导出日志，并附上设备型号；不要上传游戏文件。

## 更新

直接覆盖安装新 APK，游戏文件、存档和设置保留。**不要先卸载**：卸载会删除整个 `files` 目录。

## 法律

本项目不包含游戏代码、ROM、密钥或存档。请使用自己拥有的游戏副本；不要分发 `libmhgugame.so` / `.id`。
