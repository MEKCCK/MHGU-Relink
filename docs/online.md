# MHGU Relink：联机与 Crossplay

> 本文中的路径都相对于仓库根目录。 / Paths in this guide are relative to the repository root.


**PC 版与 Android 版共用一份 C 运行时，联网由 Rust 模块（`crates/`）实现。**

游戏代码翻译、guest 内存、内核 `svc`、HLE 服务、NVN→Vulkan 渲染、音频、媒体库全部沿用 C 基底
（仓库根目录的 `runtime/`，MHGU Relink 的完整运行时）；我们只把自己实现的**联机模块**（Rust）替换进去，
它负责 `nn::ldn`（局域网与互联网 hub）、`MHGL`/`MHGD`/`MHGR` 线协议、中继与发现。

本仓库不包含任何游戏代码、ROM、密钥或存档。游戏代码由玩家用自己的 `exefs`/`romfs` 经
`tools/` 的生成器（Python）翻译成 C，再由玩家用根目录的 `Makefile`（PC）或
`native/build_native.sh` + Gradle（Android）编成 `libmhgugame.so`。

## 目录

| 路径 | 内容 |
|---|---|
| `runtime/`、`tools/`、`third_party/`、`Makefile` | C 运行时；`runtime/net/ldn.c` 调用我们的联机库 |
| `crates/mhgu-protocol` | 线格式：`MHGS`/`MHGR`（hub）与 `MHGL`/`MHGD`（游戏内 link）、`VPacket`、`NetworkInfo` |
| `crates/mhgu-link` | link 客户端：握手、hub `LIST`/`JOIN`/`HOST`/`ATTACH`、局域网 `Scan`、`MHGD` 数据报 |
| `crates/mhgu-hub` | hub 中继服务器（Rust），协议与 PC/Android 客户端逐字节兼容；旧 Go 实现已移除 |
| `crates/mhgu-link-ffi` | 上面的 C 库：`libmhgu_link_ffi.a` + `include/mhgu_link.h`，C 运行时直接链接 |
| `crates/mhgu-crossplay` | 模拟器联机（Rust）：RyuLdn 客户端/服务器、suyu 房间客户端/服务器、混合 hub；C 运行时通过 `libmhgu_crossplay.a` 链接 |

## 构建与运行（PC）

```sh
# 1. 联机库（C 运行时要链接它；Android 构建脚本会自动交叉编译）
cargo build --release -p mhgu-link-ffi          # → target/release/libmhgu_link_ffi.a

# 2. C 运行时（仓库根目录；GEN 指向你的翻译产物，GAME 指向 exefs/romfs 所在目录）
make -j12 build/mhgu-relink GEN=<gen 目录> GAME=<dump 目录> FFMPEG=system

# 3. 运行（游戏目录含 exefs/ 与 romfs/）
cd build && ./mhgu-relink <游戏目录>
```

依赖：clang/clang++、SDL3、Vulkan、shaderc、FFmpeg（系统库即可，`FFMPEG=system`）。
`obj/`、`build/` 是构建缓存，已忽略。

## 联机

| 模式 | 做法 |
|---|---|
| 局域网 | 主机建房（日志 `[ldn] hosting via mhgu-link on port N`），其他玩家“搜索房间”或 `[Multiplayer] Join = <IP>:<port>` |
| 互联网（hub 服务器） | 跑 `./target/release/mhgu-hub -addr :39510`；所有玩家 `[Multiplayer] Server = <服务器>:39510`、相同 `Key`；主机建房会自动注册为 hub，其他人从列表进入，服务器只做中转 |

传输层全部在 `crates/mhgu-link*`：主机/加入/中继、TCP/UDP 链路、踢人、NetworkInfo 广播。C 运行时只保留
`nn::ldn` 的 HLE 包装、局域网扫描（Android 的组播锁与逐网卡探测）以及 hub 列表/检查的控制协议。

链路协议为 **v3**（上游 0.15 起）。`mhgu-hub` 额外支持：

- `-min-proto N`：拒绝更旧的链路协议（默认 0 = 不限）；
- `-separate-phones[=false]`：`false` 时 Android 的公共房间与 PC 公共房间合并；
- `-v`：记录 hub、加入与因协议被拒的请求（不记录 IP）。

### Direct（P2P 直连）

`[Multiplayer] Direct`（默认 On，设置菜单“Direct connections”）开启时，互联网 hub 的两端会各自通过中继向
服务器查询对端公网 UDP 地址（`MHGQ`）。**只有两端都开启**时服务器才回 `MHGP`（含对端的 IP:端口和本对的直连
密钥），因此 Direct = Off 的一方不会暴露地址。随后双方互发打洞包与直连 hello，路由器允许时游戏包直接 P2P，
否则继续走 `MHGR` 中继；直连 3.5 s 没有双向存活证明就回退中继并重新查询。协议与旧版兼容：旧客户端从不查询，
旧服务器会把查询当普通数据转发并被对端忽略。调试：`MHGU_LDN_RTT=1`（路径 RTT/收发包统计）、
`MHGU_LDN_BLOCK_DIRECT=s[-t]`（模拟对称 NAT）。


### Matchmaking Key（房间）

Key 是**客户端**的概念：客户端把 `Key` 算成 16 字节房间
`SHA-256("MHGURecomp matchmaking key:" + Key)[0..16]`（空 Key = 全零的公共房间），放进每个请求的头部；
服务器只比较这 16 字节，**永远看不到 Key 本身**，因此：

- 客户端侧指定 Key：C 版/gu-native 在设置里填 `Key`（F1 覆盖层 / `[Multiplayer] Key`）；
  Rust FFI 是 `mhgu_ldn_host_server(server, key)` 与 `mhgu_lobby_*(…, key)` 的 `key` 参数；
- 服务器侧限定房间：`mhgu-hub -key <同一个 Key>`。此时只有该 Key 的房间被服务，
  公共房间（空 Key）与其他 Key 的 HOST/JOIN 得到与「房间不存在」完全相同的 `T_ERROR(1)`，
  LIST 返回 0 个 hub——线上行为与「Key 不同」这种情况逐字节一致，没有任何协议扩展；
- 启动日志会打印该 Key 对应的房间前缀（`0x2a7198d5…`），后台页面显示「限定房间」；不设 `-key` 则是公共房间，
  任何使用默认 Key 的玩家都能看到并加入（默认不安全的那个状态）。

### 打包与分发

- Linux 绿色包：`dist/mhgu-hub-0.1.6-linux-x86_64.tar.gz`（二进制 + systemd unit + 示例自定义 UI）；
- Arch 包：`cd packaging/arch && makepkg -f` → `mhgu-hub-0.1.6-2-x86_64.pkg.tar.zst`；
- Windows：`packaging/windows/build.sh`（需要 `mingw-w64-gcc` 与 `rustup target add x86_64-pc-windows-gnu`）；
- WebUI 可替换：`-web-dir <目录>` 提供 `index.html` / `admin.html` / `log.html` 与静态资源，
  `/api/hubs` 的 JSON 接口不变，页面自己用 JS 取数据。

### 服务器 Web UI

`mhgu-hub` 自带一个无依赖的 Web 界面（`-web :39511`，默认开；`off` 关闭）：

- `GET /`：公开页，列「房间 1、房间 2 …」、人数、访问类型（公开 / 仅手机 / Key 房间，不显示内部 id / key 哈希）及可加入状态，另有总人数/连接数/运行时长，5 秒自动刷新；
- `GET /api/hubs`：同样的 JSON，带 `locked` 和 `access`（`public` / `phone` / `key`）；
- `GET /admin?token=…`：管理员页：统计、每个 hub 的 `Close`（`POST /admin/close`，会通知主机并踢掉该房间）、
  IP 封禁（`POST /admin/ban` / `unban`，被禁 IP 的连接立即被重置）、`/admin/log` 看最近 300 条事件；
- 界面多语言：右上角 🌐 点开是语言列表（English / 中文 / Español），点击即切换；选择写进
  `lang` cookie（`Path=/; Max-Age=1年`），之后翻页/进后台都保持，不会回到英文；
- 列表带 `key` 列（房间哈希前 4 字节，便于按 Key 分组）与流量统计（`traffic`）；
- 后台有 **Players** 表（每个已拼接连路的 pair、所属房间、玩家 IP、`↓/↑` 平均速率、`live`/`idle Ns`）与 `Kick`（`POST /admin/kick pair=…`，
  关掉该玩家两侧的 socket，pipe 线程结束、`joins` 递减）；日志可按 `?only=hubs|admin` 过滤；
- 令牌来自 `-web-token`，不填则启动时随机生成并打印。

### 热设置（WebUI，无需重启）

管理员页新增 **Live settings** 表单，`POST /admin/settings` 立即生效：

| 字段 | 作用 | 生效范围 |
|---|---|---|
| `Oldest link protocol` | 拒绝低于该链路协议的客户端 | 立即；低于新值的已有 hub 会被关闭，主机收到错误并断开 |
| `Android public room separate` | Android 公共房间是否独立 | 下一次 LIST / JOIN |
| `Verbose logs` | 记录 hub、拼接连路、拒绝原因（不记录 IP） | 立即 |
| `Max connections` / `Max connections / IP` | 连接上限 | 新连接 |
| `Max hubs` / `Max hubs / IP` | 房间上限 | 新 HOST |
| `Hubs per search` | 单次 LIST 返回上限（1–64） | 下一次 LIST |
| `Requests / second` / `Request burst` | 每 IP 请求速率 | 立即重建所有 IP 的令牌桶 |
| `Bandwidth (bytes/s)` | 每条 pair 每方向带宽 | 新建 pair；已有 pair 保持原值 |

非法值（非数字、0 上限、list > 64、带宽 < 1）整次拒绝，服务器保持原设置。
修改会写入管理员日志；所有设置也可用启动参数指定，WebUI 只负责运行中修改。

```sh
./target/release/mhgu-hub -addr :39510 -web :39511 -web-token <你的令牌> -key <房间 Key>
```

两条路径都由 `mhgu-link-ffi` 实现，与 C 版/gu-native 客户端双向兼容（LAN 直连、`MHGD` 数据报、
经服务器的 `MHGR` 中继都已验证）。

## 菜单汉化 / 字体

`runtime/overlay.cpp` 在启动时合并系统 CJK 字体（优先 JetBrains Maple Mono，`MHGU_FONT` 可覆盖），
字形范围 `GetGlyphRangesChineseSimplifiedCommon`，所以菜单切中文不会出现 `?`。

## 说明

早期版本的 Rust 运行时（`mhgu-runtime`/`mhgu-hle`/`mhgu-relink` 等）已按决定移除；现在保留的 Rust
只有联机模块与 hub 服务器。Android 版在同一个仓库的 `android/` + `native/`（C 运行时 + 同一套 Rust 联机库；见 `docs/android.md`）。

## Crossplay（Ryujinx / suyu）

`crates/mhgu-crossplay` 把上游 0.15 的模拟器联机功能移植为 Rust：`runtime/net/ldn.c` 在 `CreateNetwork` /
`Scan` / `Connect` / `Disconnect` / `SetAdvertiseData` / `Reject` 等入口按模式分支，游戏包通过
`ldn_send` 交给 RyuLdn 服务器或 suyu 房间中继。

| 设置（INI，Network 页） | 说明 |
|---|---|
| `Crossplay = Off/Ryujinx/Room/Mixed` | 关闭、Ryujinx LDN 服务器、suyu 房间、两者同时（混合 hub） |
| `RyuServers` | `host[:port]` 列表，逗号分隔；`RyuHost` 开启时也会用本机服务器 |
| `RyuHost` / `RyuPort` / `RyuPassword` | 本进程内嵌 RyuLdn 服务器（端口默认 30456）、大厅密码短语 |
| `RoomServer` | suyu 房间服务器 `host[:port]`；`RoomHost` 开启时本机内嵌 |
| `RoomHost` / `RoomPort` / `RoomName` / `RoomPassword` | 本进程内嵌房间服务器（默认 24872）、房间名与密码 |

行为与上游一致：

- `CreateNetwork` 在 Ryu/Room 模式下由服务器分配虚拟地址（`10.114.x.y` 或 `192.168.x.y`），C 侧只保留
  `nn::ldn` 状态；`GetIpv4Address` 返回服务器的子网掩码。
- `Scan` 合并 LAN、hub 服务器、RyuLdn 服务器与房间内的大厅；`Connect` 按来源选择对应链路。
- `Crossplay = Mixed`（或 Ryujinx 模式同时配置了 RoomServer/RoomHost）时，混合 hub（`bridge.rs`）会把房间玩家桥接到 Ryu 大厅，
  地址/MAC 双向改写；房间客户端与服务器协议保持逐字节兼容。
- 端口类辅助功能（`emsync`、`hpscale` 的 AUX 消息）在 crossplay 下关闭，与上游相同。

测试（无需游戏文件）：

```sh
cargo test -p mhgu-crossplay      # RyuLdn 客户端/服务器、suyu 房间、混合 hub 协议
```
