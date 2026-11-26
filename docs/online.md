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
| 互联网（hub 服务器） | 跑 `./target/release/mhgu-hub -addr :39510`；所有玩家 `[Multiplayer] Server = <服务器>:39510`、相同 `Key`；主机建房会自动注册为 hub，其他人从列表进入，服务器只做中转。只服务 Recomp 时加 `-min-proto 3`（2 及更早的 gu-native 会被拒绝） |

传输层全部在 `crates/mhgu-link*`：主机/加入/中继、TCP/UDP 链路、踢人、NetworkInfo 广播。C 运行时只保留
`nn::ldn` 的 HLE 包装、局域网扫描（Android 的组播锁与逐网卡探测）以及 hub 列表/检查的控制协议。

链路协议为 **v4**（上游 0.19 起），与旧版 **Recomp v3** 互通：link 帧不变，4 只增加 hub 服务器的能力，主机会接受任一版本的
Hello，局域网探测按对方版本回应；对方是 v3 主机时按 v3 说。协议 4 的内容：

- **集会所代码（session code）**：主机为每个集会所生成 8 字符代码（Crockford base32，无 I/L/O/U），随 `HOST`
  列出；被别的集会所占用时服务器返回 `errCodeTaken`（6），主机换一个重试；同一集会所（session id + BSSID）
  重新列出会取回原代码。设置菜单 → 网络可看到并分享，`ABCD-EFGH` 形式。
- **按代码加入**：`FIND`（请求 7）按代码找 hub，不论房间；该 hub 10 分钟内排在“加入集会所”列表首位，
  加入时带上代码即可跨房间进入（代码即邀请）。Discord Rich Presence 的 Join secret 也用它，只接受本机服务器。
- **好友（PRESENCE，请求 8 / 应答 22）**：安装身份为存档目录的 `friends-id.bin`（32 字节随机数），好友代码
  为 `SHA-256("MHGURecomp friend:" + secret)` 前 10 字节（显示 16 字符）。`friends.txt` 保存好友与已删除列表。
  服务器只在内存中保留最后 90 秒在线状态；互为好友才互见在线/开房/所在集会所代码。单向添加即好友请求，
  可接受或拒绝；拒绝在本次游戏运行内不再提示。`[Multiplayer] FriendRequests`、`FriendOnlineNotices`、
  `FriendHubNotices` 控制请求与提示。集会所内 v4 客户端还会用 `F_AUX 0xE9` 交换好友代码与猎人名。
- **HUBS4（应答 21）**：列出每个 hub 的链路版本与代码；v3 客户端仍收到旧 `HUBS`。
- **链接池**：服务器把 3 与 4 放在同一池中互相可见（`-link-pool 3-4`，可设 `off`），v4 客户端看到每个
  hub 的版本并向下兼容。`-min-proto` / `-max-proto` 可淘汰旧版本或拒绝更新的版本，二者均可在运行时修改。

`mhgu-hub` 额外支持：

- `-min-proto N` / `-max-proto N`：拒绝更旧/更新的链路协议（默认 0 = 不限 / 100）；
- `-link-pool LOW-HIGH`：互通版本区间（默认 `3-4`；`off` 各自独立）；
- `-max-presence N`：好友在线表人数上限（默认 20000）；
- `-presence-ttl SECS`：好友最后一次 PRESENCE 后保持在线的时间（默认 90 秒）；到期会向联邦 peer 广播下线。
- `-separate-phones[=false]`：`false` 时 Android 的公共房间与 PC 公共房间合并（游戏默认不再区分）；
- `-v`：记录 hub、加入与因协议被拒的请求（不记录 IP）。

### 服务器联邦（多台 hub 服务器）

多台 `mhgu-hub` 可以互相共享集会所：玩家仍连自己的服务器，搜索里会出现对端服务器的 hub，加入由本服代理，
不需要玩家切换 `Server`。配置两端互填：

```sh
mhgu-hub -federation-token 'b-token' -federation-name hub-b                 # B
mhgu-hub -federation-token 'a-token' -federate 'hub-b:39510,b-token,B'     # A（B 的 token）
```

实际部署用 Ed25519 身份与允许列表：

```sh
# B：接受 A 的身份
mhgu-hub -identity /var/lib/mhgu-hub/id -advertise hub-b.example:39510 \
         -federation-allow 'ed25519:ABCDEFGH12345678' -web :39511
# A：由 well-known 自动发现 B，或直接写地址+id
mhgu-hub -identity /var/lib/mhgu-hub/id \
         -federate 'http://hub-b.example:39511'
```

只联邦显式批准的对端；默认只列直连 peer 的 hub，`-federation-transitive N` 可让广告、代码 FIND 与加入沿最多 N 条额外
peer 转发。跨服链路固定走中继（不做 P2P），每多一跳多一次转发且两边各计流量。协议不加密，公网请套
WireGuard/VPN。`-discover lan` 只把验证过的局域网服务器列入候选表，不会自动并入。握手（Ed25519 / 兼容
共享 token）、well-known、候选嗅探、好友在线状态（含传递同步）、缓存、UDP 前缀翻译、Web Peers 页与限制见
[docs/FEDERATION.md](FEDERATION.md)。


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

- 客户端侧指定 Key：在设置里填 `Key`（F1 覆盖层 / `[Multiplayer] Key`）；
  Rust FFI 是 `mhgu_ldn_host_server(server, key)` 与 `mhgu_lobby_*(…, key)` 的 `key` 参数；
- 服务器侧限定房间：`mhgu-hub -key <同一个 Key>`。此时只有该 Key 的房间被服务，
  公共房间（空 Key）与其他 Key 的 HOST/JOIN 得到与「房间不存在」完全相同的 `T_ERROR(1)`，
  LIST 返回 0 个 hub——线上行为与「Key 不同」这种情况逐字节一致，没有任何协议扩展；
- 启动日志会打印该 Key 对应的房间前缀（`0x2a7198d5…`），后台页面显示「限定房间」；不设 `-key` 则是公共房间，
  任何使用默认 Key 的玩家都能看到并加入（默认不安全的那个状态）。

### 打包与分发

- Linux 绿色包：`dist/mhgu-hub-0.2.0-linux-x86_64.tar.gz`（二进制 + systemd unit + 示例自定义 UI）；
- Arch 包：`cd packaging/arch && makepkg -f` → `mhgu-hub-0.2.0-2-x86_64.pkg.tar.zst`；
- Windows：`packaging/windows/build.sh`（需要 `mingw-w64-gcc` 与 `rustup target add x86_64-pc-windows-gnu`）；
- WebUI 可替换：`-web-dir <目录>` 提供 `index.html` / `admin.html` / `log.html` 与静态资源，
  `/api/hubs` 的 JSON 接口不变，页面自己用 JS 取数据。

### 服务器 Web UI

`mhgu-hub` 自带一个无依赖的 Web 界面（`-web :39511`，默认开；`off` 关闭）：

- `GET /`：公开页，列房间名、玩家名、任务、密码、人数、访问类型（公开 / 仅手机 / Key 房间，不显示内部 id / key 哈希）及可加入状态，另有总人数/连接数/运行时长，5 秒自动刷新；猎人搜索不算房间，单独列在下方表格；
- `GET /api/hubs`：`hubs` 只含房间，猎人搜索在单独的 `hunter_searches` 数组；两项的条目都带 `locked`、`access`（`public` / `phone` / `key`）、`room_name`、`names`、`count`/`max`、`quest_id`/`quest_name`/`quest_goal`/`quest_rank`/`quest_stars`，以及 `quest_name_es`/`quest_name_zh`、`quest_goal_es`/`quest_goal_zh`、`in_quest`、`quest_age`（已出发秒数，不在任务中为 `null`）、`passcode`（无密码为 `null`）、`hunter_search`；`in_quest=false` 时页面显示「房间空闲」，`quest_id`/`in_quest`/`quest_age` 随主机的 UPDATE 实时刷新；
- `GET /admin?token=…`：管理员页：统计、每个 hub 的 `Close`（`POST /admin/close`，会通知主机并踢掉该房间）、
  IP 封禁（`POST /admin/ban` / `unban`，被禁 IP 的连接立即被重置）、`/admin/log` 看最近 300 条事件；
- 界面多语言：右上角 🌐 点开是语言列表（English / 中文 / Español），点击即切换；选择写进
  `lang` cookie（`Path=/; Max-Age=1年`），之后翻页/进后台都保持，不会回到英文；
- 列表带 `key` 列（房间哈希前 4 字节，便于按 Key 分组）与流量统计（`traffic`）；
- 后台有 **Players** 表（每个已拼接连路的 pair、所属房间、玩家 IP、`↓/↑` 平均速率、`live`/`idle Ns`）与 `Kick`（`POST /admin/kick pair=…`，
  关掉该玩家两侧的 socket，pipe 线程结束、`joins` 递减）；日志可按 `?only=hubs|admin` 过滤；
- 令牌来自 `-web-token`，不填则启动时随机生成并打印。

房间名、最多四个猎人名、任务 ID、是否任务中、密码和猎人搜索标志来自游戏的 `NetworkInfo` advertise 块；任务名称/目标/等级/星数由 `crates/mhgu-hub/data/quests.json` 映射，英文/西班牙文/中文分别来自 APK 的 `name`/`goal`、`name_es`/`goal_es`、`name_zh`/`goal_zh`。等级是任务等级，不是玩家等级；同名任务可能有不同 ID（例如「雪山之主」的 LR 10216 与 HR 10530）。该 JSON 由 `tools/extract_mhgudb_quests.py` 从 MHGU Database 2.4.5 的 `quests` 表提取，只保留显示所需元数据，不含 APK 或完整数据库。

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
修改会写入管理员日志；所有设置也可用启动参数指定，WebUI 只负责**本次进程**的运行中修改——重启后回到启动参数/默认值。需要重启后保持的，请写进 systemd 的启动参数，例如 `-separate-phones false`、`-min-proto 3`。

```sh
./target/release/mhgu-hub -addr :39510 -web :39511 -web-token <你的令牌> -key <房间 Key>
```

两条路径都由 `mhgu-link-ffi` 实现，只以 **MHGURecomp** 为联机兼容目标（LAN 直连、`MHGD` 数据报、
经服务器的 `MHGR` 中继都已验证）。gu-native 没有跟进 Recomp 的协议更新，不再列入兼容列表。

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
- 端口类辅助功能（`emsync`、`hpscale` 的 AUX 消息）在 crossplay 下关闭，与上游相同；延迟补偿（`hitsync`）是例外，
  它在 crossplay 下改走 UDP `47318`。

## 延迟补偿（Lag Compensation）

`[Multiplayer] LagCompensation`（默认开）让近战和弓箭/弩的命中按攻击者本地看到的结果结算：攻击者自己的游戏把命中差量
（部位伤害、状态、KO、仇恨）报告给运行怪物的主机，主机忽略该玩家副本的命中，只应用报告。协议细节：

- 非 crossplay：每个 `F_AUX`（`MAGIC 0xE7`）走 Rust 链路的可靠、有序通道；主机转发给其它站点。
- Crossplay：同样的消息作为 UDP 广播发到本地联机网络的 `47318` 端口，供模拟器侧的 Forge 延迟补偿插件互通；
  内置实现与插件不会重复报告。
- 消息：Hello（每秒及状态变化时，带 slot、开关、能力位，4 字节旧格式兼容）、命中报告（每个命中一次，含序号和怪物状态差量）、
  远程攻击单独用 shot 消息；3.5 秒听不到 Hello 视为对方关闭。混合版本会安全退化为“按主机自己的副本结算”。

测试（无需游戏文件）：

```sh
cargo test -p mhgu-crossplay      # RyuLdn 客户端/服务器、suyu 房间、混合 hub 协议
cargo test -p mhgu-link-ffi -p mhgu-hub
```
