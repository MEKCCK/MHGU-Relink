# 跨平台联机

在游戏的设置中打开“网络”，使用“跨平台联机”选择连接方式。默认值为“关”，继续使用原有集会所、LAN 搜索及中继连接。

| 模式 | 连接方式 |
| --- | --- |
| 关 | 原有重编译版集会所和中继。 |
| Ryujinx | 通过 RyuLdn 服务器建立或搜索集会所。 |
| 多人房间 | 使用 suyu 兼容的多人房间协议。 |
| 混合联机 | 将同一集会所桥接到 RyuLdn 和多人房间两端。 |

RyuLdn 服务器地址填写 `主机` 或 `主机:端口`，多个地址用逗号分隔。默认端口为 `30456`。也可开启“运行 RyuLdn 服务器”，让其他玩家填写本机地址与该端口。RyuLdn 配对口令留空表示公开；使用私密配对时，所有玩家填写相同的 `Ryujinx-` 加八位十六进制数字口令。

多人房间服务器地址填写 `主机` 或 `主机:端口`，默认端口为 `24872`。也可开启“运行多人房间服务器”。房间名称默认是 `MHGU`；需要密码时，所有玩家填写相同的房间密码。对端客户端须保留并实现相应的多人联机协议。

修改后点“保存”，离开当前集会所，再通过游戏内的“本地联机”建立或搜索集会所。正在使用的集会所保持其当前连接方式，新连接配置在下一次建房或搜索时应用。全部玩家须使用相同的 MHGU 游戏版本，当前协议按 `1.4.0` 适配。

运行本机服务器供互联网玩家连接时，对方必须能访问本机相应端口，可使用端口转发或共同的 VPN 网络。跨平台集会所会关闭重编译版专属的小怪同步和 HP 缩放；普通重编译版集会所保留原有行为。

联机库、JNI/游戏运行时和配置均从当前源码重新构建。目标游戏库 ID 必须与当前运行时一致；每次更新工具或运行时后重新生成，不使用参考 APK 中的游戏库或游戏 ID。网络代码参考 MEKCCK/MHGU-Relink，固定版本为 `a3fbb0155f8b64a5bedf6fde272a920059ad91e3`。MIT 及 Rust 依赖许可证随 APK 的 `assets/licenses/` 提供。

配置持久化、输入校验、运行时配置快照及中文标签已通过主机回归。安装包构建和签名记录请以本次交付的验证记录为准；主机回归不代表已完成手机与模拟器之间的实机联机测试。

## 本地重新构建

工具链沿用项目的 Android SDK 35、NDK 27.0.12077973、CMake 3.22.1、JDK 17、Gradle 8.12，并安装 Rust 的 aarch64-linux-android target。

在源码根目录运行：

```powershell
& "C:/Program Files/Git/bin/bash.exe" ./native/build_native.sh arm64-v8a Release
$env:JAVA_HOME = "C:/Program Files/Eclipse Adoptium/jdk-17.0.21.9-hotspot"
$env:ANDROID_HOME = "$env:LOCALAPPDATA/Android/Sdk"
& ./android/gradlew.bat -p ./android --no-daemon :app:assembleRelease
```

native 构建会自动运行 cargo build --locked，编译并静态链接 crossplay。APK 不内置游戏代码；结果位于 android/app/build/outputs/apk/release/app-release.apk。首次启动时选择含有 `exefs`、`romfs`、`libmhgugame.so` 和 `libmhgugame.id` 的游戏文件夹。

用户指定的长期签名已放在 android/mhgu.keystore 和 android/keystore.properties；现有 Gradle release 配置直接读取，两个文件均被 Git 忽略。不要将这两个文件或签名备份加入源码发布包。新证书 SHA-256 为 773546B88C066A7D5CB4C951A3D27B5A6968FBFFE2514A0553BFDE396E2F57AD，与参考 APK 一致。

Rust 协议回归：cargo test --locked -p mhgu-crossplay -- --test-threads=1。Windows C HLE 集成回归：先 cargo build --locked -p mhgu-crossplay，再运行 python tests/network/run_network_hle.py --sdl-root <SDL3开发包目录>。测试在独立工作目录中运行，不访问真实游戏存档。
