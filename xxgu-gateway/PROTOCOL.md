# 协议说明（XXGU 网关 / 本项目联机层）

本文件记录网关两侧实际使用的线格式。实现依据：Citra `network/room*`（v4）与 suyu v0.0.4
`src/network/room*.cpp`（v1，yuzu 家族）；网关只是两者之间的翻译进程。**以 suyu v0.0.4 为准**，
yuzu 旧默认值不作为校验依据。

通用约定：整数 **大端**；字符串 = `u32 长度` + UTF-8 字节；布尔 = `u8`（0/1）；IP = 4 字节；
MAC = 6 字节。传输为 ENet/UDP、channel 0、reliable。ENet 包体就是“消息本身”，**没有额外的
4 字节长度前缀**（该前缀只属于网关内部的 TCP 测试传输，不是两个房间协议的一部分）。

---

## 1. 拓扑

```
Citra + MHXX ──(Citra room v4 / ENet)──▶ 本网关 ──(Yuzu/suyu room v1 / ENet)──▶ mhgu-roomsrv
                                                                                   ▲
                                                               MHGU-Relink 玩家 ───┘
                                                               (Crossplay = 多人房间)
```

每个 Citra 客户端对应上游房间里的一个普通成员；网关自身不占成员位。两侧互不可见对方协议版本。

---

## 2. Citra 房间协议（v4）

`network_version = 4`，默认端口 24872（网关的 Citra 接入端口可另设，例如 8888）。

### 2.1 消息

| id | 名称 | 方向 | 载荷 |
|---:|---|---|---|
| 1 | JoinRequest | C→S | nickname, console_id_hash, preferred_mac(6), version(u32), password, token |
| 2 | JoinSuccess | S→C | mac(6) |
| 3 | RoomInformation | S→C | name, description, member_slots(u32), port(u16), preferred_game, host_username, count(u32), members[…] |
| 4 | SetGameInfo | C→S | name, game_id(u64) |
| 5 | WifiPacket | 双向 | type(u8), channel(u8), transmitter(6), destination(6), size(u32), data |
| 7 | ChatMessage | 双向 | nickname, username, message |
| 8/9/10/11/12/13 | NameCollision / MacCollision / VersionMismatch / WrongPassword / CloseRoom / RoomIsFull | S→C | 空；VersionMismatch 带 server_version(u32) |
| 14 | StatusMessage | S→C | type(u8 1..5), nickname, username |
| 15/16 | HostKicked / HostBanned | S→C | 空 |
| 22/23 | ModPermissionDenied / ModNoSuchUser | S→C | 空 |
| 24 | JoinSuccessAsMod | S→C | mac(6) |

`RoomInformation.members[i]` 顺序：nickname, mac(6), game_name, game_id(u64), username,
display_name, avatar_url。

`WifiPacket.type`：`Beacon=0, Data=1, Authentication=2, AssociationResponse=3,
Deauthentication=4, NodeMap=5`。UDS 的 beacon/appdata、EAPoL、关联帧都在 `data` 里。

---

## 3. Yuzu/suyu 房间协议（v1）

`network_version = 1`，默认端口 24872。消息 id 与 Citra 侧同名同号（1..24），但载荷不同。

### 3.1 消息

| id | 名称 | 方向 | 载荷 |
|---:|---|---|---|
| 1 | JoinRequest | C→S | nickname, preferred_ip(4；全 FF = 自动), version(u32=1), password, token |
| 2 | JoinSuccess | S→C | fake_ip(4) |
| 3 | RoomInformation | S→C | name, description, member_slots(u32), port(u16), preferred_game_name, host_username, count(u32), members[…] |
| 4 | SetGameInfo | C→S | name, game_id(u64), version |
| 5 | ProxyPacket | 双向 | domain(u8), local_ip(4), local_port(u16), domain(u8), remote_ip(4), remote_port(u16), protocol(u8), broadcast(u8), size(u32), data |
| 6 | LdnPacket | 双向 | type(u8), local_ip(4), remote_ip(4), broadcast(u8), size(u32), body |
| 7 | ChatMessage | S→C | nickname, username, message |
| 8..13 | NameCollision … RoomIsFull | S→C | 空 |
| 14 | StatusMessage | S→C | type(u8 1..5), nickname, username |
| 15/16/22/23/24 | HostKicked / HostBanned / ModPermissionDenied / ModNoSuchUser / JoinSuccessAsMod | S→C | 空；AsMod 与 JoinSuccess 同为 fake_ip(4) |

`RoomInformation.members[i]` 顺序：nickname, fake_ip(4), game_name, game_id(u64), game_version,
username, display_name, avatar_url。

### 3.2 ProxyPacket 的枚举（suyu v0.0.4）

- `domain`：`Unspecified=0, INET=1`（实际使用 1）。
- `protocol`：`Unspecified=0, IP=1, ICMP=2, **TCP=3, UDP=4**`。
- **本网关把 protocol 当作不透明 u8 透传**，不做范围校验（yuzu 旧实现校验 0..3，会把 UDP=4 判错）。
- `data` 是 **zstd 帧**（源端压缩）；上限按 60000 字节（解码后）与 64 KiB（压缩后）执行。

### 3.3 LdnPacket body

| type | 名称 | body 长度（实测） |
|---:|---|---:|
| 0 | Scan | 0 |
| 1 | ScanResponse | 1152（NetworkInfo） |
| 2 | Connect | 64（NodeInfo） |
| 3 | SyncNetwork | 1152（NetworkInfo） |
| 4 | Disconnect | 64（NodeInfo） |
| 5 | DestroyNetwork | 0 |

`NetworkInfo`（1152B）关键偏移：0x26 SSID、0x4B network_type、0x66 node_max、0x67 node_count、
0x68 节点表、0x26A advertise_size、0x26C advertise_data。`NodeInfo` 为 64B（IP/MAC/节点号/名称/
LocalCommunicationVersion 等）。

### 3.4 Join 顺序

- suyu v0.0.4 服务器：先 `RoomInformation`，再 `JoinSuccess`；客户端对 `JoinSuccess` 有
  “必须已收到成员表”的 soft assert。
- 本项目 `mhgu-roomsrv`（源自 morerokk/MHGURecomp）顺序相反：`JoinSuccess` → `RoomInformation`。
- 本网关兼容两种顺序：`JoinSuccess` 先到时暂存，等 `RoomInformation` 到达后再完成入厅
  （对 Citra 的 `JoinSuccess` 也延后到此时发送）。

---

## 4. UDS ↔ LDN 控制面翻译（网关职责）

- **发现**：Citra 的 `WifiPacket(Beacon)` 承载 200 字节 MHXX 厅广告 → 转成 LDN `ScanResponse`
  的 `NetworkInfo`；反向把 GU 的 `NetworkInfo` 广告转成 XX Beacon。
- **广告布局差**：GU 的密码/玩家槽比 XX 晚 8 字节（GU 有 host-id 槽）。转换按字段搬运：
  `[0x1c..0x50)` 原样、`[0x58..0x60)` → XX `[0x50..0x58)`、`[0x64..0xc2)` → XX `[0x5c..0xba)`、
  `[0xc0..0xc2)` → XX `[0xb8..0xba)`。格式标记 `u32@0x08=1, u32@0x1c=1`；
  `u32@0x20` 在 1.4.0 重编译端实测为 0，网关接受 0 或 1。
- **接入**：Citra 的 Authentication/Association → GU 侧 LDN `Connect`（64B NodeInfo）；
  GU 的 `SyncNetwork` → Citra 的 NodeMap + EAPoL-Logoff。
- **双房主**：按会话决定 Citra-host / GU-host 角色；GU 客人通过 guest injection
  （injected Authentication/EAPoL）出现在 Citra 的 node_map 中。
- 身份映射：每个 Citra MAC ↔ 上游 fake IPv4；节点号、名称、会话 id 逐项映射。

---

## 5. 与上游 yuzu 默认值的差异（本移植有意为之）

| 项 | 上游 yuzu | 本网关（suyu/实测） |
|---|---|---|
| Proxy `protocol` | 校验 0..3 | 不透明 u8；UDP=4、TCP=3 |
| `domain` | 注释称 INET=0 | 实际/采用 INET=1 |
| JoinSuccess 顺序 | 必须先 RoomInformation | 两种顺序都接受 |
| 厅广告 `0x20` 标记 | 必须 1 | 0 或 1 |
| 监听地址 | CLI 锁 127.0.0.1 | `--allow-remote-listen` 可 0.0.0.0 |
| 诊断输出 | 无 | `XXGU_GATEWAY_DEBUG=1` 才输出 |

---

## 6. 端口与安全

- Citra 接入：UDP 8888（可改）；房间：UDP 24872；二者都要在防火墙/安全组放行。
- 认证只有房间密码；密码为空即公开可进。网关没有自动重连，断线需在 Citra 内重进。
- 不支持 IPv6；房间地址必须是可达的 IPv4。

## 7. 回归测试建议

1. 用 suyu 枚举值构造 `ProxyPacket`（domain=1、protocol=3/4）验证网关不再因枚举断线；
2. 构造 `JoinSuccess` 与 `RoomInformation` 两种顺序，断言网关都能完成入厅；
3. 用 1.4.0 的真实 200B 广告（`0x20=0`）跑转换，断言生成合法 XX beacon；
4. 房间层：`crates/mhgu-crossplay/tests/gateway_room_interop.rs` 已覆盖 Join/Info/LDN/Proxy 转发。
